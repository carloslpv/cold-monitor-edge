package dev.carloslpv.coldmonitor.bridge;

import java.io.IOException;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.TimeoutException;

import com.rabbitmq.client.AMQP;
import com.rabbitmq.client.BuiltinExchangeType;
import com.rabbitmq.client.Channel;
import com.rabbitmq.client.Connection;
import com.rabbitmq.client.ConnectionFactory;
import com.rabbitmq.client.DefaultConsumer;
import com.rabbitmq.client.Envelope;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.context.SmartLifecycle;
import org.springframework.stereotype.Component;

/**
 * Consumes the devices' readings from RabbitMQ and hands each message to {@link MessageHandler}.
 * <p>
 * The devices still speak MQTT: RabbitMQ's MQTT plugin publishes every message to the {@code amq.topic} exchange,
 * with the topic's slashes turned into dots ({@code coldmonitor/cm-a1b2c3/readings} becomes the routing key
 * {@code coldmonitor.cm-a1b2c3.readings}). The bridge binds a durable quorum queue to the readings and status of
 * every device and consumes it with manual acknowledgements and a prefetch of one, so the guarantees are those of
 * the MQTT path: a message is acknowledged only after the backend took it, and while the backend is down the
 * backlog stays in the queue, in order. What the MQTT path can only log and drop goes to a dead-letter queue here.
 */
@Component
@ConditionalOnProperty(name = "cold-monitor.bridge.source", havingValue = "amqp")
class AmqpConsumer implements SmartLifecycle {

	private static final Logger log = LoggerFactory.getLogger(AmqpConsumer.class);

	static final String DEVICE_EXCHANGE = "amq.topic";

	static final String DEAD_LETTER_EXCHANGE = "coldmonitor.dead-letter";

	private final BridgeProperties.Amqp properties;

	private final BridgeProperties.Retry retry;

	private final MessageHandler handler;

	private final String topicPrefix;

	private final ConnectionFactory factory = new ConnectionFactory();

	private volatile Connection connection;

	private volatile boolean running;

	private Thread connector;

	AmqpConsumer(BridgeProperties properties, MessageHandler handler) {
		this.properties = properties.amqp();
		this.retry = properties.retry();
		this.handler = handler;
		this.topicPrefix = properties.mqtt().topicPrefix();
		try {
			factory.setUri(this.properties.uri());
		}
		catch (Exception ex) {
			throw new IllegalArgumentException("Invalid AMQP URI " + this.properties.uri() + ": " + ex.getMessage(), ex);
		}
		factory.setUsername(this.properties.username());
		if (this.properties.password() != null) {
			factory.setPassword(this.properties.password());
		}
		// after a lost connection the client reconnects, declares the topology again and resumes the consumer
		factory.setAutomaticRecoveryEnabled(true);
		factory.setTopologyRecoveryEnabled(true);
		factory.setNetworkRecoveryInterval(retry.initialDelay().toMillis());
	}

	@Override
	public void start() {
		running = true;
		// the first connection is retried here; automatic recovery only covers connections lost later
		connector = Thread.ofVirtual().name("amqp-connect").start(this::connectUntilStopped);
	}

	@Override
	public void stop() {
		running = false;
		handler.stop();
		connector.interrupt();
		Connection current = connection;
		if (current != null && current.isOpen()) {
			try {
				// unacknowledged messages go back to the queue when the channel closes
				current.close((int) Duration.ofSeconds(5).toMillis());
			}
			catch (IOException ex) {
				log.warn("Error closing the AMQP connection: {}", ex.getMessage());
			}
		}
	}

	@Override
	public boolean isRunning() {
		return running;
	}

	boolean isConnected() {
		Connection current = connection;
		return current != null && current.isOpen();
	}

	private void connectUntilStopped() {
		for (int attempt = 0; running; attempt++) {
			try {
				connection = factory.newConnection("cold-monitor-bridge");
				consume(connection.createChannel());
				log.info("Consuming {} from {}", properties.queue(), properties.uri());
				return;
			}
			catch (IOException | TimeoutException ex) {
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

	private void consume(Channel channel) throws IOException {
		channel.addShutdownListener(cause -> {
			// automatic recovery covers a lost connection, not a channel the broker closed on a live connection,
			// which RabbitMQ does when a message stays unacknowledged longer than its consumer timeout (30 minutes
			// by default, reached when the backend is down that long); the message goes back to the queue, and the
			// bridge opens a new channel and carries on
			if (running && !cause.isHardError() && !cause.isInitiatedByApplication()) {
				log.warn("RabbitMQ closed the channel ({}); opening a new one", cause.getMessage());
				Thread.ofVirtual().name("amqp-reconsume").start(() -> {
					try {
						Thread.sleep(retry.initialDelay()); // no tight loop if the broker keeps closing it
						consume(connection.createChannel());
					}
					catch (IOException ex) {
						log.error("Cannot open a new channel: {}", ex.getMessage());
					}
					catch (InterruptedException ex) {
						Thread.currentThread().interrupt();
					}
				});
			}
		});
		declareTopology(channel);
		// one message at a time: while the handler retries a reading, no other one is taken
		channel.basicQos(1);
		channel.basicConsume(properties.queue(), false, "cold-monitor-bridge", new DefaultConsumer(channel) {
			@Override
			public void handleDelivery(String consumerTag, Envelope envelope, AMQP.BasicProperties props, byte[] body)
					throws IOException {
				deliver(channel, envelope, body);
			}
		});
	}

	/**
	 * Declares what the bridge needs, idempotently, so a fresh RabbitMQ works without any setup besides the users.
	 */
	void declareTopology(Channel channel) throws IOException {
		channel.exchangeDeclare(DEAD_LETTER_EXCHANGE, BuiltinExchangeType.FANOUT, true);
		channel.queueDeclare(properties.deadLetterQueue(), true, false, false, Map.of("x-queue-type", "quorum"));
		channel.queueBind(properties.deadLetterQueue(), DEAD_LETTER_EXCHANGE, "");

		channel.queueDeclare(properties.queue(), true, false, false,
				Map.of("x-queue-type", "quorum", "x-dead-letter-exchange", DEAD_LETTER_EXCHANGE));
		channel.queueBind(properties.queue(), DEVICE_EXCHANGE, topicPrefix + ".*.readings");
		channel.queueBind(properties.queue(), DEVICE_EXCHANGE, topicPrefix + ".*.status");
	}

	private void deliver(Channel channel, Envelope envelope, byte[] body) throws IOException {
		String routingKey = envelope.getRoutingKey();
		String device = deviceOf(routingKey);
		MessageHandler.Disposition disposition;
		if (device == null) {
			log.warn("Ignoring message with unexpected routing key {}", routingKey);
			disposition = MessageHandler.Disposition.UNDELIVERABLE;
		}
		else if (routingKey.endsWith(".readings")) {
			disposition = handler.onReading(device, body, channel::isOpen);
		}
		else {
			disposition = handler.onStatus(device, body);
		}
		switch (disposition) {
			case DONE -> channel.basicAck(envelope.getDeliveryTag(), false);
			// without requeue, RabbitMQ moves it to the dead-letter exchange of the queue
			case UNDELIVERABLE -> channel.basicReject(envelope.getDeliveryTag(), false);
			case STOPPING -> {
				// left unacknowledged: it goes back to the queue when the connection closes
			}
		}
	}

	/**
	 * Device id of {@code <prefix>.<device>.readings} or {@code <prefix>.<device>.status}, or null for another
	 * routing key.
	 */
	String deviceOf(String routingKey) {
		String prefix = topicPrefix + ".";
		if (!routingKey.startsWith(prefix)) {
			return null;
		}
		String[] rest = routingKey.substring(prefix.length()).split("\\.");
		if (rest.length != 2 || rest[0].isEmpty() || !(rest[1].equals("readings") || rest[1].equals("status"))) {
			return null;
		}
		return rest[0];
	}

}
