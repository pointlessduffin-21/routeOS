# routeOS · Heartbeat Publisher
# Publishes to plant/{zone}/{machine}/status/heartbeat every 30 seconds.

/system/scripts add name="syntag-heartbeat" source={
  :local topic ("plant/" . $deviceZone . "/" . $deviceMachine . "/status/heartbeat")
  :local ts [/system/clock get time]
  :local payload ("{\"value\":true,\"unit\":\"\",\"ts\":\"" . $ts . "\",\"quality\":\"good\"}")
  /iot mqtt publish \
    broker="syntag-uns" \
    topic=$topic \
    message=$payload \
    qos=0
  :log debug ("syntag-heartbeat: " . $topic)
}

/system/scheduler add \
  name="syntag-heartbeat-sched" \
  interval=30s \
  on-event="syntag-heartbeat" \
  start-time=startup
