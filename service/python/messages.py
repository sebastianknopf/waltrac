from __future__ import annotations

import struct
import hmac
import hashlib
from typing import Optional

from abc import ABC, abstractmethod


class Payload(ABC):
	"""Abstract base class for payload types that support signing/verification.

	Subclasses must implement `_serialize_fields()` which returns the
	bytes over which the HMAC is computed (i.e. excluding the trailing HMAC).
	The concrete `serialize(key: Optional[str])` method will append either
	the calculated 16-byte HMAC (when `key` is provided) or 16 zero bytes
	(when `key` is None).
	"""

	hmac: bytes

	@abstractmethod
	def _serialize_fields(self) -> bytes:
		"""Return the byte sequence that should be signed (excluding the HMAC)."""
		pass

	def serialize(self, key: Optional[str] = None) -> bytes:
		"""Return the full byte sequence including the trailing 16-byte HMAC.

		If `key` is None, no HMAC is computed and 16 zero bytes are appended.
		If `key` is a str, compute HMAC-SHA256 over the field bytes and append
		the first 16 bytes of the digest (also stored in `self.hmac`).
		"""
		msg: bytes = self._serialize_fields()

		if key is None:
			return msg + (b"\x00" * 16)

		if not isinstance(key, str):
			raise TypeError('key must be a str')

		kb: bytes = key.encode('utf-8')
		sig: bytes = hmac.new(kb, msg, hashlib.sha256).digest()[:16]
		self.hmac = sig

		return msg + sig

	def verify(self, key: str) -> bool:
		"""Verify that `self.hmac` matches HMAC-SHA256 over the payload fields."""
		if not isinstance(self.hmac, (bytes, bytearray)) or len(self.hmac) != 16:
			return False

		if not isinstance(key, str):
			raise TypeError('key must be a str')

		kb: bytes = key.encode('utf-8')
		msg: bytes = self._serialize_fields()
		expected: bytes = hmac.new(kb, msg, hashlib.sha256).digest()[:16]

		return hmac.compare_digest(self.hmac, expected)

class Position(Payload):
	"""Represents the position payload with the following layout (big-endian/network byte order):

	- 1 byte header (bytes)
	- 1 byte interval (unsigned int)
	- 6 bytes device (bytes)
	- 4 bytes latitude (signed int, stored as int = float * 1e7)
	- 4 bytes longitude (signed int, stored as int = float * 1e7)
	- 4 bytes longitude (signed int, stored as int = float * 1e7)
	- 4 bytes timestamp (unsigned int)
	- 1 byte namelen (unsigned int)
	- n bytes name (utf-8 string)
	- 16 bytes hmac (bytes)

	The static constructor `from_bytes()` accepts the raw byte string and parses
	these fields in the order above. The empty `__init__` provides defaults.
	"""

	SCALE: float = 1e7

	# typed attributes
	header: bytes
	interval: int
	device: bytes
	latitude: float
	longitude: float
	timestamp: int
	namelen: int
	name: str
	hmac: bytes

	def __init__(self) -> None:
		# default/empty constructor with sensible defaults
		self.device = b"\x00" * 6
		self.header = b"\x00"
		self.interval = 0
		self.latitude = 0.0
		self.longitude = 0.0
		self.timestamp = 0
		self.namelen = 0
		self.name = ""
		self.hmac = b"\x00" * 16

	@staticmethod
	def init(data: bytes) -> "Position":
		if not isinstance(data, (bytes, bytearray)):
			raise TypeError('data must be bytes or bytearray')

		# minimum size without the variable-length name and hmac
		min_fixed = 1 + 1 + 6 + 4 + 4 + 4 + 1 + 16
		if len(data) < min_fixed:
			raise ValueError(f'data too short: need at least {min_fixed} bytes')

		offset = 0
		p = Position()

		# 1 byte header
		p.header = bytes(data[offset : offset + 1])
		offset += 1

		# 1 byte interval (unsigned)
		p.interval = struct.unpack_from('>B', data, offset)[0]
		offset += 1

		# 6 byte device
		p.device = bytes(data[offset : offset + 6])
		offset += 6

		# 4 byte latitude (signed int) -> float
		lat_int = struct.unpack_from('>i', data, offset)[0]
		p.latitude = float(lat_int) / p.SCALE
		offset += 4

		# 4 byte longitude (signed int) -> float
		lon_int = struct.unpack_from('>i', data, offset)[0]
		p.longitude = float(lon_int) / p.SCALE
		offset += 4

		# 4 byte timestamp (unsigned int)
		p.timestamp = struct.unpack_from('>I', data, offset)[0]
		offset += 4

		# 1 byte namelen
		p.namelen = struct.unpack_from('>B', data, offset)[0]
		offset += 1

		# n bytes name
		if len(data) < offset + p.namelen + 16:
			raise ValueError('data too short for name length and hmac')

		name_bytes = data[offset : offset + p.namelen]
		try:
			p.name = name_bytes.decode('utf-8')
		except Exception as exc:  # keep decode errors explicit
			raise ValueError('name is not valid UTF-8') from exc

		offset += p.namelen

		# 16 bytes hmac
		p.hmac = bytes(data[offset : offset + 16])
		offset += 16

		if len(data) != offset:
			raise ValueError('extra or missing bytes after parsing hmac')

		return p

	# provide the concrete implementation expected by Payload._serialize_fields()
	def _serialize_fields(self) -> bytes:
		"""Serialize all fields except the trailing HMAC (for signing/verifying)."""
		parts = bytearray()

		# order: header, interval, device, latitude, longitude, namelen, name
		parts += self.header
		parts += struct.pack('>B', int(self.interval))
		parts += self.device

		lat_int = int(round(self.latitude * self.SCALE))
		parts += struct.pack('>i', lat_int)

		lon_int = int(round(self.longitude * self.SCALE))
		parts += struct.pack('>i', lon_int)

		# timestamp: unsigned 32-bit
		parts += struct.pack('>I', int(self.timestamp))

		name_bytes = self.name.encode('utf-8')
		parts += struct.pack('>B', len(name_bytes))
		parts += name_bytes

		return bytes(parts)

	# end of Position

	def __repr__(self) -> str:  # pragma: no cover - convenience
		return (
			f"Position(header={self.header!r}, interval={self.interval}, "
			f"device={self.device!r}, latitude={self.latitude}, "
			f"longitude={self.longitude}, timestamp={self.timestamp}, name={self.name!r}, hmac={self.hmac!r})"
		)


class Command(Payload):
	"""Represents a command payload with layout (big-endian/network byte order):

	- 1 byte header (bytes)
	- 1 byte arglen (unsigned int)
	- n bytes arg (utf-8 string)
	- 16 bytes hmac (bytes)
	"""

	# typed attributes
	header: bytes
	arglen: int
	arg: str
	hmac: bytes

	def __init__(self) -> None:
		# empty/default constructor
		self.header = b"\x00"
		self.arglen = 0
		self.arg = ""
		self.hmac = b"\x00" * 16

	@staticmethod
	def init(data: bytes) -> "Command":
		if not isinstance(data, (bytes, bytearray)):
			raise TypeError('data must be bytes or bytearray')

		min_fixed = 1 + 1 + 16
		if len(data) < min_fixed:
			raise ValueError(f'data too short: need at least {min_fixed} bytes')

		offset = 0
		c = Command()

		# 1 byte header
		c.header = bytes(data[offset : offset + 1])
		offset += 1

		# 1 byte arglen
		c.arglen = struct.unpack_from('>B', data, offset)[0]
		offset += 1

		if len(data) < offset + c.arglen + 16:
			raise ValueError('data too short for arg length and hmac')

		arg_bytes = data[offset : offset + c.arglen]
		try:
			c.arg = arg_bytes.decode('utf-8')
		except Exception as exc:
			raise ValueError('arg is not valid UTF-8') from exc

		offset += c.arglen

		# 16 bytes hmac
		c.hmac = bytes(data[offset : offset + 16])
		offset += 16

		if len(data) != offset:
			raise ValueError('extra or missing bytes after parsing hmac')

		return c

	def _serialize_fields(self) -> bytes:
		"""Serialize all fields except the trailing HMAC (for signing/verifying)."""
		parts = bytearray()

		parts += self.header

		arg_bytes = self.arg.encode('utf-8')
		parts += struct.pack('>B', len(arg_bytes))

		parts += arg_bytes

		return bytes(parts)

	def __repr__(self) -> str:  # pragma: no cover - convenience
		return (
			f"Command(header={self.header!r}, arglen={self.arglen}, "
			f"arg={self.arg!r}, hmac={self.hmac!r})"
		)
