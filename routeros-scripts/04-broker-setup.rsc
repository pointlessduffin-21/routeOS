# routeOS · MQTT Broker Setup
# Run once to register the Syntag UNS broker connection.

/iot mqtt brokers add \
  name="syntag-uns" \
  address=$syntagHost \
  port=1883 \
  client-id=$deviceId \
  connect-timeout=10s \
  keepalive-interval=60s

:log info ("syntag-broker: registered syntag-uns -> " . $syntagHost)
