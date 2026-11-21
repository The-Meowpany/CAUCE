"""Runs the LoRa gateway: a serial-attached radio bridge feeding `LoRaGateway`.

    python -m cauce_server.gateway_runner --port /dev/ttyUSB0 \\
        --sync-url http://127.0.0.1:8000/v1/sync

The bridge (`pio run -e gateway`) hands up every frame its SX1276 receives. This process
reassembles batches, posts the node's signed frames to the central verbatim, and sends the
acknowledgement back down for the bridge to transmit. Neither half holds a node key.

Node signature algorithms are read from the central's database when this runs beside it, and
refreshed periodically so a node provisioned later is picked up without a restart.
"""

from __future__ import annotations

import argparse
import logging
import time
import urllib.error
import urllib.request

from . import serial_bridge
from .lora_gateway import GatewayError, LoRaGateway

log = logging.getLogger("cauce.gateway")

ALGORITHM_REFRESH_S = 300


class HttpTransport:
    """The `post(url, body, headers) -> (status, text)` shape `LoRaGateway` expects."""

    def __init__(self, timeout_s: float = 2.0):
        # Short on purpose: the node waits about three seconds for its acknowledgement, and
        # an answer that arrives after it gave up is airtime spent for nothing.
        self.timeout_s = timeout_s

    def post(self, url: str, body: bytes, headers: dict) -> tuple[int, str]:
        request = urllib.request.Request(url, data=body, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
                return response.status, response.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read().decode("utf-8", "replace")
        except (urllib.error.URLError, TimeoutError) as exc:
            return 0, str(exc)


class GatewayRunner:
    """Moves bytes between a serial port and a `LoRaGateway`.

    `port` needs `read(n) -> bytes` (returning what is available, possibly nothing) and
    `write(data)`. pyserial's `Serial` with a short timeout is one; the tests use a fake.
    """

    def __init__(self, gateway: LoRaGateway, port):
        self.gateway = gateway
        self.port = port
        self.decoder = serial_bridge.Decoder()
        self.acks_sent = 0
        self.relay_failures = 0
        self.last_status = ""
        self.last_rssi: int | None = None

    def poll(self) -> int:
        """Processes whatever the port has. Returns how many bridge frames were handled."""
        data = self.port.read(512)
        if not data:
            return 0
        frames = self.decoder.feed(data)
        for frame in frames:
            self._handle(frame)
        return len(frames)

    def _handle(self, frame: serial_bridge.BridgeFrame) -> None:
        if frame.frame_type == serial_bridge.STATUS:
            self.last_status = frame.payload.decode("ascii", "replace")
            log.info("bridge: %s", self.last_status)
            return
        if frame.frame_type != serial_bridge.RADIO_RECEIVED:
            return
        try:
            rssi, lora = frame.radio_frame()
        except ValueError:
            return
        self.last_rssi = rssi
        try:
            ack = self.gateway.on_frame(lora)
        except GatewayError as exc:
            # No acknowledgement: the node keeps the batch and retries, which is the whole
            # point of acknowledging only what the central durably stored.
            self.relay_failures += 1
            log.warning("relay failed: %s", exc)
            return
        if ack:
            self.port.write(serial_bridge.encode(serial_bridge.RADIO_TRANSMIT, ack))
            self.acks_sent += 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="CAUCE LoRa gateway runner")
    parser.add_argument("--port", required=True, help="serial port of the gateway bridge")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--sync-url", default="http://127.0.0.1:8000/v1/sync")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

    import serial  # pyserial; imported here so the central does not need it to run

    gateway = LoRaGateway(args.sync_url, HttpTransport())
    port = serial.Serial(args.port, args.baud, timeout=0.05)
    runner = GatewayRunner(gateway, port)
    known = gateway.load_algorithms_from_database()
    log.info("gateway on %s -> %s, %d node algorithms known", args.port, args.sync_url, known)
    next_refresh = time.monotonic() + ALGORITHM_REFRESH_S
    while True:
        runner.poll()
        if time.monotonic() >= next_refresh:
            gateway.load_algorithms_from_database()
            next_refresh = time.monotonic() + ALGORITHM_REFRESH_S


if __name__ == "__main__":
    raise SystemExit(main())
