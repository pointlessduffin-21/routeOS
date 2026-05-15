# routeOS · Telemetry Publisher
# Publishes cpu_load, free_memory, uptime_seconds, interface_errors every 5 seconds.

/system/scripts add name="syntag-telemetry" source={
  :local ts [/system/clock get time]
  :local base ("plant/" . $deviceZone . "/" . $deviceMachine)
  :local cpu [/system/resource get cpu-load]
  :local mem [/system/resource get free-memory]
  :local up  [/system/resource get uptime]
  :local rxErr [/interface/ethernet get ($deviceInterface) rx-error]
  :local txErr [/interface/ethernet get ($deviceInterface) tx-error]

  /iot mqtt publish broker="syntag-uns" \
    topic=($base . "/cpu_load") \
    message=("{\"value\":" . $cpu . ",\"unit\":\"%\",\"ts\":\"" . $ts . "\",\"quality\":\"good\"}") \
    qos=1

  /iot mqtt publish broker="syntag-uns" \
    topic=($base . "/free_memory") \
    message=("{\"value\":" . $mem . ",\"unit\":\"KB\",\"ts\":\"" . $ts . "\",\"quality\":\"good\"}") \
    qos=1

  /iot mqtt publish broker="syntag-uns" \
    topic=($base . "/uptime_seconds") \
    message=("{\"value\":\"" . $up . "\",\"unit\":\"s\",\"ts\":\"" . $ts . "\",\"quality\":\"good\"}") \
    qos=1

  /iot mqtt publish broker="syntag-uns" \
    topic=($base . "/interface_errors") \
    message=("{\"rx_errors\":" . $rxErr . ",\"tx_errors\":" . $txErr . ",\"ts\":\"" . $ts . "\",\"quality\":\"good\"}") \
    qos=1
}

/system/scheduler add \
  name="syntag-telemetry-sched" \
  interval=5s \
  on-event="syntag-telemetry" \
  start-time=startup
