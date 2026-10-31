package dev.carloslpv.coldmonitor.bridge;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.ConfigurationPropertiesScan;

@SpringBootApplication
@ConfigurationPropertiesScan
public class ColdMonitorBridgeApplication {

	public static void main(String[] args) {
		SpringApplication.run(ColdMonitorBridgeApplication.class, args);
	}

}
