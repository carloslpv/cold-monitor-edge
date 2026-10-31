package dev.carloslpv.coldmonitor.bridge;

/**
 * The backend could not take the reading now (network error, 5xx, 429). The reading is kept and tried again.
 */
class BackendUnavailableException extends Exception {

	BackendUnavailableException(String message, Throwable cause) {
		super(message, cause);
	}

}
