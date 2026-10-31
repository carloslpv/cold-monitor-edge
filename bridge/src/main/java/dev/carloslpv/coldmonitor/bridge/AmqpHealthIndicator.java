package dev.carloslpv.coldmonitor.bridge;

import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.boot.health.contributor.Health;
import org.springframework.boot.health.contributor.HealthIndicator;
import org.springframework.stereotype.Component;

/**
 * Health is down while the bridge has no RabbitMQ connection, so Docker and monitoring notice a bridge that runs
 * but receives nothing.
 */
@Component("amqp")
@ConditionalOnProperty(name = "cold-monitor.bridge.source", havingValue = "amqp")
class AmqpHealthIndicator implements HealthIndicator {

	private final AmqpConsumer consumer;

	private final BridgeMetrics metrics;

	AmqpHealthIndicator(AmqpConsumer consumer, BridgeMetrics metrics) {
		this.consumer = consumer;
		this.metrics = metrics;
	}

	@Override
	public Health health() {
		var health = consumer.isConnected() ? Health.up() : Health.down();
		return health.withDetail("devicesOnline", metrics.devicesOnline()).build();
	}

}
