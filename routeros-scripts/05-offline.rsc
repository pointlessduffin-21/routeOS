# routeOS · Offline Notification
# Sends HTTP PUT to set device status=offline on shutdown.

/system/scripts add name="syntag-offline" source={
  :local url ("http://" . $syntagHost . ":" . $syntagESP32Port . "/api/devices/" . $deviceId)
  /tool fetch \
    url=$url \
    http-method=put \
    http-header-field="Content-Type: application/json" \
    http-data="{\"status\":\"offline\"}" \
    output=none \
    as-value
  :log info "syntag-offline: status set to offline"
}

/system/scheduler add \
  name="syntag-offline-sched" \
  on-event="syntag-offline" \
  interval=0 \
  start-time=shutdown
