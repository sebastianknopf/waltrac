import asyncio
import logging
import random
import string

from paho.mqtt import client


class MqttPublisher:
    def __init__(self, host: str, port: int, username: str|None, password: str|None, toplevel: str = 'waltrac'):
        self._host = host
        self._port = port
        self._username = username
        self._password = password
        self._keepalive = 60

        self._toplevel = toplevel
        self._connected = asyncio.Event()
        self._task: asyncio.Task | None = None
        self._mqtt: client.Client|None = None

    async def start(self):
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(self._run())

    async def stop(self):
        self._mqtt.loop_stop()
        self._mqtt.disconnect()

        if self._task is not None and not self._task.done():
            self._task.cancel()

            try:
                await self._task
            except asyncio.CancelledError:
                pass

        self._task = None

    async def _run(self):
        client_id_seed: str = ''.join(random.choice(string.ascii_letters + string.digits) for _ in range(8))
        client_id: str = f"waltrac-gateway-{client_id_seed}"

        self._mqtt: client.Client = client.Client(client.CallbackAPIVersion.VERSION2, protocol=client.MQTTv5, client_id=client_id)

        if self._username is not None and self._password is not None:
            self._mqtt.username_pw_set(username=self._username, password=self._password)

        self._mqtt.connect(self._host, self._port)
        self._mqtt.loop_start()

        logging.info(f"Connected to MQTT broker at {self._host}:{self._port}.")
        self._connected.set()

    async def publish(self, topic: str, payload: bytes):
        await self._connected.wait()
        
        try:
            self._mqtt.publish(f"{self._toplevel}/{topic}", payload)
        except Exception:
            self._connected.clear()
            
            if self._task:
                self._task.cancel()
            
            raise