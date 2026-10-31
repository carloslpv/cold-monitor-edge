package dev.carloslpv.coldmonitor.bridge;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import tools.jackson.core.JacksonException;
import tools.jackson.databind.json.JsonMapper;

import org.springframework.stereotype.Component;

/**
 * Handles one MQTT message. It returns {@code true} when the message is done with (forwarded, a duplicate or a bad
 * message that is dropped) and may be acknowledged to the broker, and {@code false} when the bridge is stopping
 * before it could forward it: the message is then left unacknowledged, and the broker delivers it again after the
 * restart.
 * <p>
 * While the backend is unavailable the handler retries the same reading with growing delays. That blocks the MQTT
 * callback thread on purpose: no further message is taken, the broker keeps the backlog in the bridge's persistent
 * session, and the readings arrive in order once the backend is back.
 */
@Component
class MessageHandler {

	private static final Logger log = LoggerFactory.getLogger(MessageHandler.class);

	private final ReadingForwarder forwarder;

	private final BridgeMetrics metrics;

	private final JsonMapper json;

	private final BridgeProperties.Retry retry;

	private final CountDownLatch stopping = new CountDownLatch(1);

	MessageHandler(ReadingForwarder forwarder, BridgeMetrics metrics, JsonMapper json, BridgeProperties properties) {
		this.forwarder = forwarder;
		this.metrics = metrics;
		this.json = json;
		this.retry = properties.retry();
	}

	boolean onReading(String device, byte[] payload) {
		DeviceReading reading;
		try {
			reading = json.readValue(payload, DeviceReading.class);
		}
		catch (JacksonException ex) {
			return drop("invalid", "Dropping unreadable reading from {}: {}", device, ex.getOriginalMessage());
		}
		if (!reading.isComplete()) {
			return drop("invalid", "Dropping incomplete reading from {}: {}", device, reading);
		}
		if (!reading.device().equals(device)) {
			// the topic is what the broker ACL checks, so a payload naming another device is not trusted
			return drop("invalid", "Dropping reading published by {} in the name of {}", device, reading.device());
		}

		for (int attempt = 0;; attempt++) {
			try {
				var outcome = forwarder.forward(reading);
				metrics.reading(outcome.name().toLowerCase(Locale.ROOT));
				return true;
			}
			catch (BackendUnavailableException ex) {
				metrics.retry();
				Duration delay = retry.delay(attempt);
				log.warn("{}; retrying reading {} in {} s", ex.getMessage(), reading.deliveryKey(), delay.toSeconds());
				if (!sleep(delay)) {
					return false;
				}
			}
		}
	}

	boolean onStatus(String device, byte[] payload) {
		String status = new String(payload, StandardCharsets.UTF_8).trim();
		switch (status) {
			case "online" -> log.info("Device {} is online", device);
			case "offline" -> log.warn("Device {} went offline", device);
			default -> {
				log.warn("Ignoring unknown status '{}' of device {}", status, device);
				return true;
			}
		}
		metrics.deviceStatus(device, status.equals("online"));
		return true;
	}

	/**
	 * Ends the retry loops; called when the application shuts down.
	 */
	void stop() {
		stopping.countDown();
	}

	private boolean sleep(Duration delay) {
		try {
			return !stopping.await(delay.toMillis(), TimeUnit.MILLISECONDS);
		}
		catch (InterruptedException ex) {
			Thread.currentThread().interrupt();
			return false;
		}
	}

	private boolean drop(String outcome, String message, Object... args) {
		log.warn(message, args);
		metrics.reading(outcome);
		return true;
	}

}
