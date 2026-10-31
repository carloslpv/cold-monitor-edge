package dev.carloslpv.coldmonitor.bridge;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.TimeoutException;

import com.rabbitmq.client.Channel;
import com.rabbitmq.client.Connection;
import com.rabbitmq.client.ConnectionFactory;
import com.rabbitmq.client.GetResponse;
import io.micrometer.core.instrument.MeterRegistry;
import org.eclipse.paho.client.mqttv3.MqttClient;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.testcontainers.images.builder.Transferable;
import org.testcontainers.junit.jupiter.Container;
import org.testcontainers.junit.jupiter.Testcontainers;
import org.testcontainers.rabbitmq.RabbitMQContainer;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.system.CapturedOutput;
import org.springframework.boot.test.system.OutputCaptureExtension;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

import static org.assertj.core.api.Assertions.assertThat;

/**
 * The RabbitMQ path with a real broker: the test publishes over MQTT like a device, RabbitMQ's MQTT plugin routes
 * the message to the bridge's queue, and the bridge consumes it over AMQP and forwards it to a fake backend.
 */
@SpringBootTest
@Testcontainers
@ExtendWith(OutputCaptureExtension.class)
class AmqpBridgeIntegrationTests {

	private static final Duration WAIT = Duration.ofSeconds(15);

	private static final String PROBE = "28ff4a1c0b160351";

	private static final UUID FREEZER = UUID.fromString("3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90");

	private static final String API_KEY = "test-device-api-key-0123456789";

	private static final int MQTT_PORT = 1883;

	@Container
	static final RabbitMQContainer rabbit = new RabbitMQContainer("rabbitmq:4.1-management")
		.withCopyToContainer(Transferable.of("[rabbitmq_management,rabbitmq_mqtt]."), "/etc/rabbitmq/enabled_plugins")
		// check consumer timeouts every second instead of every minute, for the test that waits for one
		.withCopyToContainer(Transferable.of("[{rabbit, [{channel_tick_interval, 1000}]}]."),
				"/etc/rabbitmq/advanced.config")
		.withExposedPorts(5672, 15672, MQTT_PORT);

	private static final FakeBackend backend = startBackend();

	@Autowired
	private AmqpConsumer consumer;

	@Autowired
	private BridgeMetrics metrics;

	@Autowired
	private MeterRegistry meters;

	private MqttClient device;

	private Connection inspector;

	@DynamicPropertySource
	static void properties(DynamicPropertyRegistry registry) {
		registry.add("cold-monitor.bridge.source", () -> "amqp");
		registry.add("cold-monitor.bridge.amqp.uri", rabbit::getAmqpUrl);
		registry.add("cold-monitor.bridge.amqp.username", rabbit::getAdminUsername);
		registry.add("cold-monitor.bridge.amqp.password", rabbit::getAdminPassword);
		registry.add("cold-monitor.bridge.backend.url", backend::url);
		registry.add("cold-monitor.bridge.backend.api-key", () -> API_KEY);
		registry.add("cold-monitor.bridge.probes", () -> PROBE + "=" + FREEZER);
		registry.add("cold-monitor.bridge.retry.initial-delay", () -> "100ms");
		registry.add("cold-monitor.bridge.retry.max-delay", () -> "200ms");
	}

	@BeforeEach
	void connect() throws Exception {
		awaitBridgeConnected();
		backend.reset();
		var options = new MqttConnectOptions();
		options.setUserName(rabbit.getAdminUsername());
		options.setPassword(rabbit.getAdminPassword().toCharArray());
		device = new MqttClient("tcp://" + rabbit.getHost() + ":" + rabbit.getMappedPort(MQTT_PORT), "test-device",
				new MemoryPersistence());
		device.connect(options);

		var factory = new ConnectionFactory();
		factory.setUri(rabbit.getAmqpUrl());
		factory.setUsername(rabbit.getAdminUsername());
		factory.setPassword(rabbit.getAdminPassword());
		inspector = factory.newConnection("test-inspector");
		try (Channel channel = inspector.createChannel()) {
			channel.queuePurge("coldmonitor.bridge.dead");
		}
	}

	@AfterEach
	void disconnect() throws Exception {
		device.disconnect();
		device.close();
		inspector.close();
	}

	@AfterAll
	static void stopBackend() {
		backend.close();
	}

	@Test
	void forwardsAReadingPublishedOverMqtt() throws Exception {
		publishReading("cm-01", 1, 1, PROBE, "4.25", 1761868800L);

		var request = backend.nextRequest(WAIT);
		assertThat(request).isNotNull();
		assertThat(request.path()).isEqualTo("/api/v1/equipment/" + FREEZER + "/readings");
		assertThat(request.apiKey()).isEqualTo(API_KEY);
		assertThat(request.body()).isEqualTo("{\"temperature\":4.25,\"recordedAt\":\"2025-10-31T00:00:00Z\"}");
	}

	@Test
	void keepsTheBacklogInOrderWhileTheBackendIsDown() throws Exception {
		backend.answerNext(503, 503, 201);

		publishReading("cm-01", 2, 1, PROBE, "7.50", null);
		publishReading("cm-01", 2, 2, PROBE, "7.75", null);

		// prefetch of one: the second reading waits in the queue until the first one got through
		for (int attempt = 1; attempt <= 3; attempt++) {
			assertThat(backend.nextRequest(WAIT)).as("attempt %d", attempt)
				.isNotNull()
				.satisfies(request -> assertThat(request.body()).contains("7.50"));
		}
		assertThat(backend.nextRequest(WAIT).body()).contains("7.75");
		assertThat(deadLetters()).isEmpty();
	}

	@Test
	void carriesOnWhenRabbitMQTakesBackAReadingLeftUnacknowledgedTooLong(CapturedOutput output) throws Exception {
		rabbit.execInContainer("rabbitmqctl", "set_policy", "short-consumer-timeout", "^coldmonitor\\.bridge$",
				"{\"consumer-timeout\":2000}", "--apply-to", "queues");
		try {
			int[] down = new int[2000];
			Arrays.fill(down, 503);
			backend.answerNext(down);
			double forwardedBefore = count("forwarded");

			publishReading("cm-01", 4, 1, PROBE, "8.25", null);

			// the backend stays down past the timeout: RabbitMQ closes the channel and delivers the reading again on
			// the bridge's new channel, and the copy that was being retried gives up
			long deadline = System.nanoTime() + Duration.ofSeconds(90).toNanos();
			while (!(output.getOut().contains("RabbitMQ closed the channel")
					&& output.getOut().contains("Giving up on reading cm-01/4/1")) && System.nanoTime() < deadline) {
				Thread.sleep(100);
			}
			assertThat(output.getOut()).contains("RabbitMQ closed the channel", "Giving up on reading cm-01/4/1");

			backend.reset(); // back up
			assertThat(backend.nextRequest(WAIT).body()).contains("8.25");
			assertThat(backend.nextRequest(Duration.ofSeconds(1))).as("only one copy kept trying").isNull();
			assertThat(count("forwarded") - forwardedBefore).isEqualTo(1);

			publishReading("cm-01", 4, 2, PROBE, "8.50", null);
			assertThat(backend.nextRequest(WAIT).body()).contains("8.50");
		}
		finally {
			rabbit.execInContainer("rabbitmqctl", "clear_policy", "short-consumer-timeout");
			backend.reset();
		}
	}

	@Test
	void sendsWhatItCannotForwardToTheDeadLetterQueue() throws Exception {
		publishReading("cm-01", 3, 1, "28aaaaaaaaaaaaaa", "4.00", null); // probe not registered
		publish("coldmonitor/cm-01/readings", "not json");
		backend.answerNext(400); // rejected by the backend
		publishReading("cm-01", 3, 2, PROBE, "999", null);
		publishReading("cm-01", 3, 3, PROBE, "5.00", null);

		assertThat(backend.nextRequest(WAIT).body()).contains("999");
		assertThat(backend.nextRequest(WAIT).body()).contains("5.00");
		List<String> dead = awaitDeadLetters(3);
		assertThat(dead).hasSize(3);
		assertThat(dead.get(0)).contains("28aaaaaaaaaaaaaa");
		assertThat(dead.get(1)).isEqualTo("not json");
		assertThat(dead.get(2)).contains("999");
	}

	@Test
	void tracksWhichDevicesAreOnline() throws Exception {
		publish("coldmonitor/cm-20/status", "online");
		publish("coldmonitor/cm-21/status", "online");
		publish("coldmonitor/cm-21/status", "offline");

		long deadline = System.nanoTime() + WAIT.toNanos();
		while (!Boolean.FALSE.equals(metrics.status("cm-21")) && System.nanoTime() < deadline) {
			Thread.sleep(50);
		}
		assertThat(metrics.status("cm-20")).isTrue();
		assertThat(metrics.status("cm-21")).isFalse();
		assertThat(consumer.deviceOf("coldmonitor.cm-20.status")).isEqualTo("cm-20");
		assertThat(consumer.deviceOf("coldmonitor.cm-20.readings")).isEqualTo("cm-20");
		assertThat(consumer.deviceOf("coldmonitor.cm-20.config")).isNull();
		assertThat(consumer.deviceOf("coldmonitor.cm-20.x.readings")).isNull();
		assertThat(consumer.deviceOf("elsewhere.cm-20.status")).isNull();
	}

	private double count(String outcome) {
		var counter = meters.find("coldmonitor.bridge.readings").tag("outcome", outcome).counter();
		return counter != null ? counter.count() : 0;
	}

	private List<String> awaitDeadLetters(int expected) throws Exception {
		List<String> dead = new ArrayList<>();
		long deadline = System.nanoTime() + WAIT.toNanos();
		while (dead.size() < expected && System.nanoTime() < deadline) {
			dead.addAll(deadLetters());
			Thread.sleep(50);
		}
		return dead;
	}

	private List<String> deadLetters() throws IOException, TimeoutException {
		List<String> dead = new ArrayList<>();
		try (Channel channel = inspector.createChannel()) {
			GetResponse message;
			while ((message = channel.basicGet("coldmonitor.bridge.dead", true)) != null) {
				dead.add(new String(message.getBody(), StandardCharsets.UTF_8));
			}
		}
		return dead;
	}

	private void publishReading(String deviceId, long boot, long seq, String probe, String t, Long ts)
			throws MqttException {
		String json = "{\"device\":\"%s\",\"boot\":%d,\"seq\":%d,\"probe\":\"%s\",\"t\":%s%s}".formatted(deviceId,
				boot, seq, probe, t, ts != null ? ",\"ts\":" + ts : "");
		publish("coldmonitor/" + deviceId + "/readings", json);
	}

	private void publish(String topic, String payload) throws MqttException {
		device.publish(topic, payload.getBytes(StandardCharsets.UTF_8), 1, false);
	}

	private void awaitBridgeConnected() throws InterruptedException {
		long deadline = System.nanoTime() + WAIT.toNanos();
		while (!consumer.isConnected() && System.nanoTime() < deadline) {
			Thread.sleep(50);
		}
		assertThat(consumer.isConnected()).as("bridge connected to RabbitMQ").isTrue();
		Thread.sleep(200); // let the queue and its bindings be declared before publishing
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
