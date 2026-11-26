"""示例插件：从 stdin 读一行 JSON，向 stdout 写一行 JSON。

接口约定：
  输入  {"sn": "...", "workOrder": "...", "product": "...", "step": "...", "params": {...}}
  输出  {"pass": true|false, "message": "...", "measures": {"value": 12.3}}

让主程序调用本插件：在 station.json 的 plugins.enabled 里加入 "example_python"，
并在方案中使用 {"action": "plugin", "params": {"name": "example_python"}}。
"""

import json
import sys


def main() -> int:
    line = sys.stdin.readline()
    try:
        context = json.loads(line) if line.strip() else {}
    except json.JSONDecodeError:
        print(json.dumps({"pass": False, "message": "输入不是合法 JSON"}))
        return 0

    params = context.get("params") or {}
    expected = float(params.get("expected", 5.0))
    measured = float(params.get("measured", 5.0))

    print(json.dumps({
        "pass": abs(measured - expected) <= 0.05,
        "message": f"实测 {measured}，期望 {expected}",
        "measures": {"value": measured},
    }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
