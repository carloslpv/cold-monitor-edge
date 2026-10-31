package dev.carloslpv.coldmonitor.bridge;

import java.net.URI;
import java.time.Duration;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.boot.context.properties.bind.DefaultValue;
import org.springframework.util.StringUtils;

/**
 * @param mqtt broker connection
 * @param backend cold-monitor backend that receives the readings
 * @param probes which equipment each probe measures, as {@code <probe ROM code>=<equipment id>} entries; a reading
 * from a probe that is not listed is dropped and counted, so a new probe is registered here before it goes live
 * @param retry how long to wait between attempts while the backend is unavailable
 * @param dedupCapacity how many recently forwarded readings are remembered to drop duplicates
 */
@ConfigurationProperties("cold-monitor.bridge")
public record BridgeProperties(
		@DefaultValue Mqtt mqtt,
		@DefaultValue Backend backend,
		@DefaultValue List<String> probes,
		@DefaultValue Retry retry,
		@DefaultValue("10000") int dedupCapacity) {

	public BridgeProperties {
		probes = List.copyOf(probes);
		probeMap(probes); // fail at startup on a malformed entry, not at the first reading
	}

	/**
	 * Equipment id per probe, keyed by lowercase ROM code.
	 */
	public Map<String, UUID> equipmentByProbe() {
		return probeMap(probes);
	}

	private static Map<String, UUID> probeMap(List<String> entries) {
		Map<String, UUID> map = new HashMap<>();
		for (String entry : entries) {
			if (!StringUtils.hasText(entry)) {
				continue;
			}
			String[] parts = entry.split("=", 2);
			if (parts.length != 2 || !StringUtils.hasText(parts[0])) {
				throw new IllegalArgumentException("Probe entries look like <probe ROM code>=<equipment id>, got '"
						+ entry + "' (cold-monitor.bridge.probes / BRIDGE_PROBES)");
			}
			map.put(parts[0].trim().toLowerCase(Locale.ROOT), UUID.fromString(parts[1].trim()));
		}
		return Map.copyOf(map);
	}

	/**
	 * @param uri broker URI, {@code tcp://} or {@code ssl://}
	 * @param clientId client id of the bridge; it must stay the same across restarts, because the broker keeps the
	 * session (subscriptions and unacknowledged messages) under this id
	 * @param username broker user
	 * @param password broker password
	 * @param topicPrefix first topic level, shared with the firmware
	 */
	public record Mqtt(
			@DefaultValue("tcp://localhost:1883") String uri,
			@DefaultValue("cold-monitor-bridge") String clientId,
			String username,
			String password,
			@DefaultValue("coldmonitor") String topicPrefix) {
	}

	/**
	 * @param url base URL of the backend API
	 * @param apiKey device key of the backend ({@code DEVICE_API_KEY} there), sent in {@code X-API-Key}
	 * @param timeout connect and read timeout of each request
	 */
	public record Backend(
			@DefaultValue("http://localhost:8080") URI url,
			String apiKey,
			@DefaultValue("10s") Duration timeout) {

		public Backend {
			if (!StringUtils.hasText(apiKey)) {
				throw new IllegalArgumentException(
						"Set BACKEND_API_KEY (cold-monitor.bridge.backend.api-key) to the DEVICE_API_KEY of the backend");
			}
		}
	}

	/**
	 * @param initialDelay wait after the first failure
	 * @param maxDelay longest wait; the delay doubles up to it
	 */
	public record Retry(
			@DefaultValue("1s") Duration initialDelay,
			@DefaultValue("1m") Duration maxDelay) {

		Duration delay(int attempt) {
			Duration delay = initialDelay;
			for (int i = 0; i < attempt && delay.compareTo(maxDelay) < 0; i++) {
				delay = delay.multipliedBy(2);
			}
			return delay.compareTo(maxDelay) > 0 ? maxDelay : delay;
		}
	}

}
