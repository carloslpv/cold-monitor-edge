package dev.carloslpv.coldmonitor.bridge;

import java.math.BigDecimal;
import java.net.http.HttpClient;
import java.time.Instant;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.springframework.http.HttpStatus;
import org.springframework.http.HttpStatusCode;
import org.springframework.http.MediaType;
import org.springframework.http.client.JdkClientHttpRequestFactory;
import org.springframework.stereotype.Component;
import org.springframework.web.client.ResourceAccessException;
import org.springframework.web.client.RestClient;

/**
 * Posts readings to the backend ({@code POST /api/v1/equipment/{id}/readings}) and decides what each answer means:
 * accepted, a permanent rejection (dropped, retrying would fail forever) or a temporary failure (retried).
 */
@Component
class ReadingForwarder {

	private static final Logger log = LoggerFactory.getLogger(ReadingForwarder.class);

	enum Outcome {
		FORWARDED, DUPLICATE, UNKNOWN_PROBE, REJECTED
	}

	private final RestClient client;

	private final Map<String, UUID> equipmentByProbe;

	private final Map<String, Boolean> recent;

	ReadingForwarder(RestClient.Builder builder, BridgeProperties properties) {
		var backend = properties.backend();
		var requestFactory = new JdkClientHttpRequestFactory(
				HttpClient.newBuilder().connectTimeout(backend.timeout()).build());
		requestFactory.setReadTimeout(backend.timeout());
		this.client = builder.baseUrl(backend.url().toString())
			.requestFactory(requestFactory)
			.defaultHeader("X-API-Key", backend.apiKey())
			.build();
		this.equipmentByProbe = properties.equipmentByProbe();
		int capacity = properties.dedupCapacity();
		// least recently forwarded entries go first once the map is full
		this.recent = Collections.synchronizedMap(new LinkedHashMap<>(capacity, 0.75f, true) {
			@Override
			protected boolean removeEldestEntry(Map.Entry<String, Boolean> eldest) {
				return size() > capacity;
			}
		});
	}

	Outcome forward(DeviceReading reading) throws BackendUnavailableException {
		if (recent.containsKey(reading.deliveryKey())) {
			return Outcome.DUPLICATE;
		}
		UUID equipmentId = equipmentByProbe.get(reading.probe().toLowerCase(Locale.ROOT));
		if (equipmentId == null) {
			log.warn("Dropping reading from unregistered probe {} of device {}", reading.probe(), reading.device());
			return Outcome.UNKNOWN_PROBE;
		}

		var body = new ReadingRequest(reading.t(), reading.ts() != null ? Instant.ofEpochSecond(reading.ts()) : null);
		HttpStatusCode status;
		try {
			status = client.post()
				.uri("/api/v1/equipment/{id}/readings", equipmentId)
				.contentType(MediaType.APPLICATION_JSON)
				.body(body)
				.exchange((request, response) -> response.getStatusCode());
		}
		catch (ResourceAccessException ex) {
			throw new BackendUnavailableException("Backend unreachable: " + ex.getMessage(), ex);
		}

		if (status.is2xxSuccessful()) {
			recent.put(reading.deliveryKey(), Boolean.TRUE);
			return Outcome.FORWARDED;
		}
		if (status.is5xxServerError() || status.value() == HttpStatus.TOO_MANY_REQUESTS.value()) {
			throw new BackendUnavailableException("Backend answered " + status.value(), null);
		}
		if (status.value() == HttpStatus.UNAUTHORIZED.value() || status.value() == HttpStatus.FORBIDDEN.value()) {
			// a wrong key is a configuration error, not a bad reading: hold the readings until it is fixed
			throw new BackendUnavailableException(
					"Backend refused the API key (" + status.value() + "), check BACKEND_API_KEY", null);
		}
		// 400 (invalid temperature) or 404 (equipment removed): sending it again cannot help
		log.error("Backend rejected reading {} of equipment {} with {}", reading.deliveryKey(), equipmentId,
				status.value());
		return Outcome.REJECTED;
	}

	/**
	 * Body of the backend endpoint; {@code recordedAt} null means "use the server time".
	 */
	record ReadingRequest(BigDecimal temperature, Instant recordedAt) {
	}

}
