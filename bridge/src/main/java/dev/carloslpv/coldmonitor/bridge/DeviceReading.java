package dev.carloslpv.coldmonitor.bridge;

import java.math.BigDecimal;

/**
 * A reading as the firmware publishes it on {@code coldmonitor/<device>/readings}.
 *
 * @param device device id, also the second topic level
 * @param boot random id the device draws at every boot
 * @param seq sequence number, restarts at 1 on every boot
 * @param probe 1-Wire ROM code of the probe, 16 hex digits
 * @param t temperature in °C with two decimals
 * @param ts Unix time in seconds when the reading was taken, absent if the device clock was not set
 */
record DeviceReading(String device, Long boot, Long seq, String probe, BigDecimal t, Long ts) {

	boolean isComplete() {
		return device != null && boot != null && seq != null && probe != null && t != null;
	}

	/**
	 * Identifies the reading across redeliveries: the device sends again whatever the broker did not acknowledge, so
	 * the same reading can arrive twice. {@code boot} keeps seq 1 of a new boot apart from seq 1 of the last one.
	 */
	String deliveryKey() {
		return device + "/" + boot + "/" + seq;
	}

}
