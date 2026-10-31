package dev.carloslpv.coldmonitor.bridge;

import java.time.Duration;

import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken;
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended;
import org.eclipse.paho.client.mqttv3.MqttClient;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.context.SmartLifecycle;
import org.springframework.stereotype.Component;
import org.springframework.util.StringUtils;

/**
 * Keeps the MQTT connection and hands each message to {@link MessageHandler}.
 * <p>
 * Delivery is at least once from end to end. The bridge connects with a persistent session ({@code cleanSession =
 * false}) and acknowledges a message only after the backend took it, so a reading that arrives while the bridge or
 * the backend is down waits in the broker instead of being lost. The price is that a reading can arrive twice, which
 * {@link ReadingForwarder} filters out.
 */
@Component
@ConditionalOnProperty(name = "cold-monitor.bridge.source", havingValue = "mqtt", matchIfMissing = true)
class MqttSubscriber implements SmartLifecycle, MqttCallbackExtended {

	private static final Logger log = LoggerFactory.getLogger(MqttSubscriber.class);

	private static final int QOS = 1;

	private static final Duration DISCONNECT_TIMEOUT = Duration.ofSeconds(5);

	private final BridgeProperties.Mqtt properties;

	private final BridgeProperties.Retry retry;

	private final MessageHandler handler;

	private final String readingsFilter;

	private final String statusFilter;

	private volatile MqttClient client;

	private volatile boolean running;

	private Thread connector;

	MqttSubscriber(BridgeProperties properties, MessageHandler handler) {
		this.properties = properties.mqtt();
		this.retry = properties.retry();
		this.handler = handler;
		this.readingsFilter = this.properties.topicPrefix() + "/+/readings";
		this.statusFilter = this.properties.topicPrefix() + "/+/status";
	}

	@Override
	public void start() {
		try {
			client = new MqttClient(properties.uri(), properties.clientId(), new MemoryPersistence());
		}
		catch (MqttException ex) {
			throw new IllegalStateException("Invalid MQTT settings: " + ex.getMessage(), ex);
		}
		client.setCallback(this);
		client.setManualAcks(true);
		running = true;
		// the first connection is retried here; Paho's automatic reconnect only covers connections lost later
		connector = Thread.ofVirtual().name("mqtt-connect").start(this::connectUntilStopped);
	}

	@Override
	public void stop() {
		running = false;
		handler.stop();
		connector.interrupt();
		try {
			if (client.isConnected()) {
				client.disconnect(DISCONNECT_TIMEOUT.toMillis());
			}
			client.close();
		}
		catch (MqttException ex) {
			log.warn("Error closing the MQTT connection: {}", ex.getMessage());
		}
	}

	@Override
	public boolean isRunning() {
		return running;
	}

	boolean isConnected() {
		return client != null && client.isConnected();
	}

	private void connectUntilStopped() {
		var options = new MqttConnectOptions();
		options.setCleanSession(false);
		options.setAutomaticReconnect(true);
		options.setKeepAliveInterval(30);
		options.setMaxReconnectDelay((int) retry.maxDelay().toMillis());
		if (StringUtils.hasText(properties.username())) {
			options.setUserName(properties.username());
			options.setPassword(properties.password() != null ? properties.password().toCharArray() : new char[0]);
		}
		for (int attempt = 0; running; attempt++) {
			try {
				client.connect(options);
				return; // subscribing happens in connectComplete
			}
			catch (MqttException ex) {
				Duration delay = retry.delay(attempt);
				log.warn("Cannot connect to {} ({}); retrying in {} s", properties.uri(), ex.getMessage(),
						delay.toSeconds());
				try {
					Thread.sleep(delay);
				}
				catch (InterruptedException interrupted) {
					return;
				}
			}
		}
	}

	@Override
	public void connectComplete(boolean reconnect, String serverUri) {
		log.info("{} to {}", reconnect ? "Reconnected" : "Connected", serverUri);
		// subscribing from the callback thread would wait on that same thread, so it runs on a thread of its own;
		// the broker keeps the subscriptions of a persistent session, so doing it again on reconnect is harmless
		Thread.ofVirtual().name("mqtt-subscribe").start(() -> {
			try {
				client.subscribe(new String[] { readingsFilter, statusFilter }, new int[] { QOS, QOS });
				log.info("Subscribed to {} and {}", readingsFilter, statusFilter);
			}
			catch (MqttException ex) {
				log.error("Subscription failed: {}", ex.getMessage());
			}
		});
	}

	@Override
	public void connectionLost(Throwable cause) {
		log.warn("MQTT connection lost: {}", cause.getMessage());
	}

	@Override
	public void messageArrived(String topic, MqttMessage message) throws MqttException {
		String device = deviceOf(topic);
		MessageHandler.Disposition disposition;
		if (device == null) {
			log.warn("Ignoring message on unexpected topic {}", topic);
			disposition = MessageHandler.Disposition.DONE;
		}
		else if (topic.endsWith("/readings")) {
			disposition = handler.onReading(device, message.getPayload());
		}
		else {
			disposition = handler.onStatus(device, message.getPayload());
		}
		// MQTT has no dead-letter queue: an undeliverable reading was logged and counted, and is acknowledged
		if (disposition != MessageHandler.Disposition.STOPPING) {
			client.messageArrivedComplete(message.getId(), message.getQos());
		}
	}

	@Override
	public void deliveryComplete(IMqttDeliveryToken token) {
		// the bridge does not publish
	}

	/**
	 * Device id of {@code <prefix>/<device>/readings} or {@code <prefix>/<device>/status}, or null for another
	 * topic.
	 */
	String deviceOf(String topic) {
		String prefix = properties.topicPrefix() + "/";
		if (!topic.startsWith(prefix)) {
			return null;
		}
		String[] rest = topic.substring(prefix.length()).split("/");
		if (rest.length != 2 || rest[0].isEmpty() || !(rest[1].equals("readings") || rest[1].equals("status"))) {
			return null;
		}
		return rest[0];
	}

}
