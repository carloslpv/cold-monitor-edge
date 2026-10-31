package dev.carloslpv.coldmonitor.bridge;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Deque;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentLinkedDeque;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

import com.sun.net.httpserver.HttpServer;

/**
 * Stands in for the cold-monitor backend: records every request and answers with the statuses it was told to,
 * 201 once none are left.
 */
class FakeBackend implements AutoCloseable {

	record Request(String path, String apiKey, String body) {
	}

	private final HttpServer server;

	private final BlockingQueue<Request> requests = new LinkedBlockingQueue<>();

	private final Deque<Integer> statuses = new ConcurrentLinkedDeque<>();

	FakeBackend() throws IOException {
		server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
		server.createContext("/", exchange -> {
			String body = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
			requests.add(new Request(exchange.getRequestURI().getPath(),
					exchange.getRequestHeaders().getFirst("X-API-Key"), body));
			Integer status = statuses.poll();
			exchange.sendResponseHeaders(status != null ? status : 201, -1);
			exchange.close();
		});
		server.start();
	}

	String url() {
		return "http://127.0.0.1:" + server.getAddress().getPort();
	}

	void answerNext(int... codes) {
		for (int code : codes) {
			statuses.add(code);
		}
	}

	Request nextRequest(Duration timeout) throws InterruptedException {
		return requests.poll(timeout.toMillis(), TimeUnit.MILLISECONDS);
	}

	void reset() {
		requests.clear();
		statuses.clear();
	}

	@Override
	public void close() {
		server.stop(0);
	}

}
