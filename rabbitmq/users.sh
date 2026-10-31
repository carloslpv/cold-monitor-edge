#!/bin/sh
# Creates the users of compose.rabbitmq.yaml, with the same rights mosquitto/acl gives on the Mosquitto setup.
# The password is read from standard input, so it does not show up in the shell history or the process list.
#
# The operator account is the "admin" user RabbitMQ creates on its first start (RABBITMQ_ADMIN_PASSWORD).
#
#   ./rabbitmq/users.sh bridge             the bridge: its queues and the bindings to amq.topic
#   ./rabbitmq/users.sh device cm-a1b2c3   a device: its own readings, status and config only
#
# RABBITMQ_CONTAINER=<name> runs rabbitmqctl in that container instead of the compose service.
set -eu

ctl() {
	if [ -n "${RABBITMQ_CONTAINER:-}" ]; then
		docker exec -i "$RABBITMQ_CONTAINER" rabbitmqctl "$@"
	else
		docker compose -f "$(dirname "$0")/../compose.rabbitmq.yaml" exec -T rabbitmq rabbitmqctl "$@"
	fi
}

read_password() {
	if [ -t 0 ]; then
		printf 'password for %s: ' "$1" >&2
		stty -echo
		IFS= read -r password
		stty echo
		echo >&2
	else
		IFS= read -r password
	fi
	[ -n "$password" ] || { echo "empty password" >&2; exit 1; }
}

add_user() {
	read_password "$1"
	ctl delete_user "$1" >/dev/null 2>&1 || true
	printf '%s\n' "$password" | ctl add_user "$1"
}

case "${1:-}" in
bridge)
	add_user bridge
	# configure, write and read: its own queues and the dead-letter exchange, plus reading from amq.topic
	ctl set_permissions -p / bridge '^coldmonitor\.' '^coldmonitor\.' '^(amq\.topic|coldmonitor\..*)$'
	;;
device)
	device="${2:?usage: users.sh device <device id>}"
	add_user "$device"
	# the MQTT plugin keeps each client's subscription in a queue named mqtt-subscription-<client id>...
	ctl set_permissions -p / "$device" '^mqtt-subscription-' '^(amq\.topic|mqtt-subscription-.*)$' \
		'^(amq\.topic|mqtt-subscription-.*)$'
	# ...and the topic permissions are what keep a device to its own topics, like the %u patterns of mosquitto/acl
	ctl set_topic_permissions -p / "$device" amq.topic '^coldmonitor\.{username}\.(readings|status)$' \
		'^coldmonitor\.{username}\.config$'
	;;
*)
	sed -n '2,10p' "$0" >&2
	exit 1
	;;
esac
