package dev.carloslpv.coldmonitor.bridge;

import org.springframework.boot.health.contributor.Health;
import org.springframework.boot.health.contributor.HealthIndicator;
import org.springframework.stereotype.Component;

/**
 * Health is down while the bridge has no broker connection, so Docker and monitoring notice a bridge that runs but
 * receives nothing.
 */
@Component("mqtt")
class MqttHealthIndicator implements HealthIndicator {

	private final MqttSubscriber subscriber;

	private final BridgeMetrics metrics;

	MqttHealthIndicator(MqttSubscriber subscriber, BridgeMetrics metrics) {
		this.subscriber = subscriber;
		this.metrics = metrics;
	}

	@Override
	public Health health() {
		var health = subscriber.isConnected() ? Health.up() : Health.down();
		return health.withDetail("devicesOnline", metrics.devicesOnline()).build();
	}

}
