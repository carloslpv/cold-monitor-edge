package dev.carloslpv.coldmonitor.bridge;

import java.io.IOException;
import java.net.ServerSocket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Duration;
import java.util.Properties;
import java.util.UUID;

import io.micrometer.core.instrument.MeterRegistry;
import io.moquette.broker.Server;
import io.moquette.broker.config.MemoryConfig;
import org.eclipse.paho.client.mqttv3.MqttClient;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

import static org.assertj.core.api.Assertions.assertThat;

/**
 * The whole path with real MQTT and real HTTP: an embedded broker, the bridge, and a fake backend.
 */
@SpringBootTest
class BridgeIntegrationTests {

	private static final Duration WAIT = Duration.ofSeconds(10);

	private static final String PROBE = "28ff4a1c0b160351";

	private static final UUID FREEZER = UUID.fromString("3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90");

	private static final String API_KEY = "test-device-api-key-0123456789";

	private static final int BROKER_PORT = freePort();

	private static final Server broker = startBroker();

	private static final FakeBackend backend = startBackend();

	@Autowired
	private MqttSubscriber subscriber;

	@Autowired
	private MeterRegistry meters;

	@Autowired
	private BridgeMetrics metrics;

	private MqttClient device;

	@DynamicPropertySource
	static void properties(DynamicPropertyRegistry registry) {
		registry.add("cold-monitor.bridge.mqtt.uri", () -> "tcp://127.0.0.1:" + BROKER_PORT);
		registry.add("cold-monitor.bridge.mqtt.username", () -> "");
		registry.add("cold-monitor.bridge.backend.url", backend::url);
		registry.add("cold-monitor.bridge.backend.api-key", () -> API_KEY);
		registry.add("cold-monitor.bridge.probes", () -> PROBE + "=" + FREEZER);
		registry.add("cold-monitor.bridge.retry.initial-delay", () -> "100ms");
		registry.add("cold-monitor.bridge.retry.max-delay", () -> "200ms");
	}

	@BeforeEach
	void connectDevice() throws Exception {
		awaitBridgeConnected();
		backend.reset();
		device = new MqttClient("tcp://127.0.0.1:" + BROKER_PORT, "test-device", new MemoryPersistence());
		device.connect();
	}

	@AfterEach
	void disconnectDevice() throws MqttException {
		device.disconnect();
		device.close();
	}

	@AfterAll
	static void stopServers() {
		backend.close();
		broker.stopServer();
	}

	@Test
	void forwardsAReadingToItsEquipment() throws Exception {
		publishReading("cm-01", 1, 1, PROBE, "4.25", 1761868800L);

		var request = backend.nextRequest(WAIT);
		assertThat(request).isNotNull();
		assertThat(request.path()).isEqualTo("/api/v1/equipment/" + FREEZER + "/readings");
		assertThat(request.apiKey()).isEqualTo(API_KEY);
		assertThat(request.body()).isEqualTo("{\"temperature\":4.25,\"recordedAt\":\"2025-10-31T00:00:00Z\"}");
	}

	@Test
	void leavesTheTimeToTheBackendWhenTheDeviceClockWasNotSet() throws Exception {
		publishReading("cm-01", 2, 1, PROBE, "-18.05", null);

		assertThat(backend.nextRequest(WAIT).body()).isEqualTo("{\"temperature\":-18.05,\"recordedAt\":null}");
	}

	@Test
	void forwardsARedeliveredReadingOnlyOnce() throws Exception {
		publishReading("cm-01", 3, 1, PROBE, "4.00", null);
		publishReading("cm-01", 3, 1, PROBE, "4.00", null); // PUBACK lost, the device sends it again
		publishReading("cm-01", 3, 2, PROBE, "4.10", null);

		assertThat(backend.nextRequest(WAIT).body()).contains("4.00");
		assertThat(backend.nextRequest(WAIT).body()).contains("4.10");
		assertThat(backend.nextRequest(Duration.ofMillis(300))).isNull();
		assertThat(count("duplicate")).isPositive();
	}

	@Test
	void keepsRetryingWhileTheBackendIsDown() throws Exception {
		backend.answerNext(503, 503, 201);

		publishReading("cm-01", 4, 1, PROBE, "7.50", null);

		for (int attempt = 1; attempt <= 3; attempt++) {
			assertThat(backend.nextRequest(WAIT)).as("attempt %d", attempt).isNotNull().satisfies(
					request -> assertThat(request.body()).contains("7.50"));
		}
		assertThat(meters.counter("coldmonitor.bridge.retries").count()).isGreaterThanOrEqualTo(2);
	}

	@Test
	void dropsWhatItCannotForwardAndCarriesOn() throws Exception {
		publishReading("cm-01", 5, 1, "28aaaaaaaaaaaaaa", "4.00", null); // probe not registered
		publish("coldmonitor/cm-01/readings", "not json");
		publishReading("cm-02", 5, 2, PROBE, "4.00", null, "coldmonitor/cm-01/readings"); // wrong device
		backend.answerNext(400); // rejected by the backend
		publishReading("cm-01", 5, 3, PROBE, "999", null);
		publishReading("cm-01", 5, 4, PROBE, "5.00", null);

		assertThat(backend.nextRequest(WAIT).body()).contains("999");
		assertThat(backend.nextRequest(WAIT).body()).contains("5.00");
		assertThat(count("unknown_probe")).isPositive();
		assertThat(count("invalid")).isGreaterThanOrEqualTo(2);
		assertThat(count("rejected")).isPositive();
	}

	@Test
	void tracksWhichDevicesAreOnline() throws Exception {
		publish("coldmonitor/cm-10/status", "online");
		publish("coldmonitor/cm-11/status", "online");
		publish("coldmonitor/cm-11/status", "offline");

		long deadline = System.nanoTime() + WAIT.toNanos();
		while (!Boolean.FALSE.equals(metrics.status("cm-11")) && System.nanoTime() < deadline) {
			Thread.sleep(50);
		}
		assertThat(metrics.status("cm-10")).isTrue();
		assertThat(metrics.status("cm-11")).isFalse();
		assertThat(meters.get("coldmonitor.bridge.devices.online").gauge().value()).isEqualTo(1);
		assertThat(subscriber.deviceOf("coldmonitor/cm-10/status")).isEqualTo("cm-10");
		assertThat(subscriber.deviceOf("coldmonitor/cm-10/other")).isNull();
		assertThat(subscriber.deviceOf("elsewhere/cm-10/status")).isNull();
	}

	private double count(String outcome) {
		var counter = meters.find("coldmonitor.bridge.readings").tag("outcome", outcome).counter();
		return counter != null ? counter.count() : 0;
	}

	private void publishReading(String deviceId, long boot, long seq, String probe, String t, Long ts)
			throws MqttException {
		publishReading(deviceId, boot, seq, probe, t, ts, "coldmonitor/" + deviceId + "/readings");
	}

	private void publishReading(String deviceId, long boot, long seq, String probe, String t, Long ts, String topic)
			throws MqttException {
		String json = "{\"device\":\"%s\",\"boot\":%d,\"seq\":%d,\"probe\":\"%s\",\"t\":%s%s}".formatted(deviceId,
				boot, seq, probe, t, ts != null ? ",\"ts\":" + ts : "");
		publish(topic, json);
	}

	private void publish(String topic, String payload) throws MqttException {
		device.publish(topic, payload.getBytes(StandardCharsets.UTF_8), 1, false);
	}

	private void awaitBridgeConnected() throws InterruptedException {
		long deadline = System.nanoTime() + WAIT.toNanos();
		while (!subscriber.isConnected() && System.nanoTime() < deadline) {
			Thread.sleep(50);
		}
		assertThat(subscriber.isConnected()).as("bridge connected to the broker").isTrue();
		Thread.sleep(200); // let the subscription land before publishing
	}

	private static int freePort() {
		try (var socket = new ServerSocket(0)) {
			return socket.getLocalPort();
		}
		catch (IOException ex) {
			throw new IllegalStateException(ex);
		}
	}

	private static Server startBroker() {
		var config = new Properties();
		config.setProperty("host", "127.0.0.1");
		config.setProperty("port", String.valueOf(BROKER_PORT));
		config.setProperty("allow_anonymous", "true");
		config.setProperty("persistence_enabled", "false");
		try {
			config.setProperty("data_path", Files.createTempDirectory("moquette").toString());
		}
		catch (IOException ex) {
			throw new IllegalStateException(ex);
		}
		var server = new Server();
		try {
			server.startServer(new MemoryConfig(config));
		}
		catch (IOException ex) {
			throw new IllegalStateException(ex);
		}
		return server;
	}

	private static FakeBackend startBackend() {
		try {
			return new FakeBackend();
		}
		catch (IOException ex) {
			throw new IllegalStateException(ex);
		}
	}

}
