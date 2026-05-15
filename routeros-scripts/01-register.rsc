# routeOS · Self-Registration Script
# Runs on boot via scheduler. Calls POST /api/devices to register this device.

/system/scripts add name="syntag-register" source={
  :local url ("http://" . $syntagHost . ":" . $syntagESP32Port . "/api/devices")
  :local payload ("{\"id\":\"" . $deviceId . "\",\"name\":\"" . $deviceName . "\",\"zone\":\"" . $deviceZone . "\",\"machine\":\"" . $deviceMachine . "\",\"firmware\":\"" . $deviceFirmware . "\",\"status\":\"online\",\"configuredMetrics\":[\"cpu_load\",\"free_memory\",\"uptime_seconds\",\"interface_errors\"]}")
  /tool fetch \
    url=$url \
    http-method=post \
    http-header-field="Content-Type: application/json" \
    http-data=$payload \
    output=none \
    as-value
  :log info ("syntag-register: registered at " . $url)
}

/system/scheduler add \
  name="syntag-register-sched" \
  on-event="syntag-register" \
  interval=0 \
  start-time=startup
