package dev.carloslpv.coldmonitor.bridge;

import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.UUID;

import org.junit.jupiter.api.Test;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatIllegalArgumentException;

class BridgePropertiesTests {

	private static final UUID FREEZER = UUID.fromString("3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90");

	@Test
	void mapsProbesCaseInsensitively() {
		var properties = properties(List.of(" 28FF4A1C0B160351 = " + FREEZER, ""));

		assertThat(properties.equipmentByProbe()).containsExactly(java.util.Map.entry("28ff4a1c0b160351", FREEZER));
	}

	@Test
	void rejectsMalformedProbeEntries() {
		assertThatIllegalArgumentException().isThrownBy(() -> properties(List.of("28ff4a1c0b160351")))
			.withMessageContaining("BRIDGE_PROBES");
		assertThatIllegalArgumentException().isThrownBy(() -> properties(List.of("28ff4a1c0b160351=not-a-uuid")));
	}

	@Test
	void requiresTheBackendKey() {
		assertThatIllegalArgumentException()
			.isThrownBy(() -> new BridgeProperties.Backend(URI.create("http://localhost:8080"), " ", Duration.ofSeconds(1)))
			.withMessageContaining("BACKEND_API_KEY");
	}

	@Test
	void retryDelayDoublesUpToTheMaximum() {
		var retry = new BridgeProperties.Retry(Duration.ofSeconds(1), Duration.ofSeconds(10));

		assertThat(retry.delay(0)).isEqualTo(Duration.ofSeconds(1));
		assertThat(retry.delay(3)).isEqualTo(Duration.ofSeconds(8));
		assertThat(retry.delay(4)).isEqualTo(Duration.ofSeconds(10));
		assertThat(retry.delay(100)).isEqualTo(Duration.ofSeconds(10));
	}

	private static BridgeProperties properties(List<String> probes) {
		return new BridgeProperties(
				new BridgeProperties.Mqtt("tcp://localhost:1883", "bridge", null, null, "coldmonitor"),
				new BridgeProperties.Backend(URI.create("http://localhost:8080"), "key-0123456789abcdef",
						Duration.ofSeconds(1)),
				probes, new BridgeProperties.Retry(Duration.ofSeconds(1), Duration.ofMinutes(1)), 100);
	}

}
