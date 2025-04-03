# kmsg2sock

kmsg2sock is a toy Linux kernel module that exposes kernel messages (from dmesg/kmsg) over a TCP server.

## Overview

This module starts a simple TCP server that listens on 0.0.0.0:2244 (by default) and streams the kernel ring buffer to connected clients. It can be useful for debugging or educational purposes when experimenting with kernel development or logging mechanisms.

One particular use case: in scenarios where the system experiences catastrophic failures (e.g., root disk corruption) but the kernel is still alive, this module allows you to remotely retrieve critical logs that may help with diagnostics.

> ⚠️ Note: This is not intended for production use. For production systems, consider using the in-kernel [netconsole](https://www.kernel.org/doc/html/latest/networking/netconsole.html) facility, which reliably forwards kernel logs over UDP to a pre-configured remote address.

## Features

- Exposes kernel logs via a simple TCP stream.
- Default listening address: 0.0.0.0:2244.
- Installed and managed via DKMS.

## Usage

### Install with DKMS

1. Copy the source to /usr/src/kmsg2sock-<version> (e.g., /usr/src/kmsg2sock-0.1/)
2. Add the module to DKMS:

```sh
sudo dkms add -m kmsg2sock -v 0.1
```

3. Build and install:

```sh
sudo dkms build -m kmsg2sock -v 0.1
sudo dkms install -m kmsg2sock -v 0.1
```

4. Load the module:

```sh
sudo modprobe kmsg2sock
```

5. Connect to the TCP log stream:

```sh
nc <host-ip> 2244
```

### Uninstall

```sh
sudo dkms remove -m kmsg2sock -v 0.1 --all
```
