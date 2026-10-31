package dev.carloslpv.coldmonitor.bridge;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.MeterRegistry;

import org.springframework.stereotype.Component;

/**
 * Readings by outcome, retries and devices online, so a Grafana panel shows at a glance whether data is flowing.
 */
@Component
class BridgeMetrics {

	private final MeterRegistry registry;

	private final Counter retries;

	private final Map<String, Boolean> online = new ConcurrentHashMap<>();

	BridgeMetrics(MeterRegistry registry) {
		this.registry = registry;
		this.retries = Counter.builder("coldmonitor.bridge.retries")
			.description("Attempts to forward a reading that failed because the backend was unavailable")
			.register(registry);
		Gauge.builder("coldmonitor.bridge.devices.online", online, map -> map.values().stream().filter(b -> b).count())
			.description("Devices whose last status message was online")
			.register(registry);
	}

	void reading(String outcome) {
		Counter.builder("coldmonitor.bridge.readings")
			.description("Readings received from MQTT, by what happened to them")
			.tag("outcome", outcome)
			.register(registry)
			.increment();
	}

	void retry() {
		retries.increment();
	}

	void deviceStatus(String device, boolean isOnline) {
		online.put(device, isOnline);
	}

	/**
	 * Last status of a device: true online, false offline, null when it never sent one.
	 */
	Boolean status(String device) {
		return online.get(device);
	}

	long devicesOnline() {
		return online.values().stream().filter(b -> b).count();
	}

}
