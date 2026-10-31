package dev.carloslpv.coldmonitor.bridge;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import tools.jackson.core.JacksonException;
import tools.jackson.databind.json.JsonMapper;

import org.springframework.stereotype.Component;

/**
 * Handles one message from the broker, whichever protocol brought it, and says what to do with it (see
 * {@link Disposition}).
 * <p>
 * While the backend is unavailable the handler retries the same reading with growing delays. That blocks the
 * consumer thread on purpose: no further message is taken, the broker keeps the backlog (in the bridge's persistent
 * MQTT session, or in its RabbitMQ queue), and the readings arrive in order once the backend is back.
 */
@Component
class MessageHandler {

	private static final Logger log = LoggerFactory.getLogger(MessageHandler.class);

	private final ReadingForwarder forwarder;

	private final BridgeMetrics metrics;

	private final JsonMapper json;

	private final BridgeProperties.Retry retry;

	private final CountDownLatch stopping = new CountDownLatch(1);

	enum Disposition {

		/** Forwarded, or a duplicate: acknowledge it. */
		DONE,

		/**
		 * Cannot be forwarded, now or later (unreadable, from an unregistered probe, rejected by the backend). Over
		 * AMQP it goes to the dead-letter queue, where it can be inspected and replayed; MQTT has no such thing, so
		 * there it is acknowledged and only logged and counted.
		 */
		UNDELIVERABLE,

		/**
		 * The bridge is stopping, or the broker took the message back, before it could be forwarded: leave it
		 * unacknowledged, and the broker delivers it again.
		 */
		STOPPING

	}

	MessageHandler(ReadingForwarder forwarder, BridgeMetrics metrics, JsonMapper json, BridgeProperties properties) {
		this.forwarder = forwarder;
		this.metrics = metrics;
		this.json = json;
		this.retry = properties.retry();
	}

	Disposition onReading(String device, byte[] payload) {
		return onReading(device, payload, () -> true);
	}

	/**
	 * Like {@link #onReading(String, byte[])}, giving up between retries once {@code stillOwned} turns false: RabbitMQ
	 * takes back a message left unacknowledged too long and delivers it again, and two copies must not both keep
	 * trying.
	 */
	Disposition onReading(String device, byte[] payload, BooleanSupplier stillOwned) {
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
			if (!stillOwned.getAsBoolean()) {
				log.info("Giving up on reading {}, the broker delivers it again", reading.deliveryKey());
				return Disposition.STOPPING;
			}
			try {
				var outcome = forwarder.forward(reading);
				metrics.reading(outcome.name().toLowerCase(Locale.ROOT));
				return switch (outcome) {
					case FORWARDED, DUPLICATE -> Disposition.DONE;
					case UNKNOWN_PROBE, REJECTED -> Disposition.UNDELIVERABLE;
				};
			}
			catch (BackendUnavailableException ex) {
				metrics.retry();
				Duration delay = retry.delay(attempt);
				log.warn("{}; retrying reading {} in {} s", ex.getMessage(), reading.deliveryKey(), delay.toSeconds());
				if (!sleep(delay)) {
					return Disposition.STOPPING;
				}
			}
		}
	}

	Disposition onStatus(String device, byte[] payload) {
		String status = new String(payload, StandardCharsets.UTF_8).trim();
		switch (status) {
			case "online" -> log.info("Device {} is online", device);
			case "offline" -> log.warn("Device {} went offline", device);
			default -> {
				log.warn("Ignoring unknown status '{}' of device {}", status, device);
				return Disposition.DONE;
			}
		}
		metrics.deviceStatus(device, status.equals("online"));
		return Disposition.DONE;
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

	private Disposition drop(String outcome, String message, Object... args) {
		log.warn(message, args);
		metrics.reading(outcome);
		return Disposition.UNDELIVERABLE;
	}

}
