#!/usr/bin/env python3
"""Bridge newline JSON controls from the native M6.1 UI to the M6 socket."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS.parent))
from tools.external_control_client import ControlConnection  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, type=Path)
    parser.add_argument("--token", required=True, type=Path)
    parser.add_argument("--client-id", default="m6-keyboard-client")
    arguments = parser.parse_args()
    connection = None
    try:
        connection = ControlConnection(
            arguments.socket, arguments.token, arguments.client_id
        )
        for line in sys.stdin:
            value = json.loads(line)
            if value.get("action") == "stop":
                break
            connection.command(
                float(value["throttle"]), float(value["brake"]), float(value["steering"])
            )
        connection.command(0.0, 1.0, 0.0)
        connection.release()
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(
            json.dumps(
                {
                    "source": "keyboard_control_bridge",
                    "event": "bridge_failed",
                    "error": str(error),
                }
            ),
            flush=True,
        )
        return 2
    finally:
        if connection is not None:
            connection.close()


if __name__ == "__main__":
    raise SystemExit(main())
