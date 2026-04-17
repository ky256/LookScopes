#!/usr/bin/env python3
"""CodeBuddy OpenAI 兼容代理服务。

在本地监听 HTTP 请求，将 Cursor 发来的标准 OpenAI API 请求
转发到 CodeBuddy（glm-5.0），自动注入认证 Header 和动态刷新 JWT。

用法:
    python3 infra/hooks/codebuddy_proxy.py [--port 5678]

Cursor 配置:
    Override OpenAI Base URL → http://localhost:5678/v1
"""

import glob
import json
import os
import ssl
import sys
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

CODEBUDDY_ENDPOINT = "https://copilot.tencent.com/v2/chat/completions"
DEFAULT_PORT = 5678
REQUEST_TIMEOUT = 120


def find_codebuddy_auth() -> dict | None:
    """从本地 CodeBuddy 认证文件读取最新 JWT 和用户信息。"""
    # Windows: %LOCALAPPDATA%\CodeBuddyExtension\...
    # macOS:   ~/Library/Application Support/CodeBuddyExtension\...
    local_app = os.environ.get("LOCALAPPDATA", "")
    if not local_app and sys.platform == "darwin":
        local_app = os.path.expanduser(
            "~/Library/Application Support"
        )

    if not local_app:
        return None

    exact_path = os.path.join(
        local_app, "CodeBuddyExtension", "Data", "Public",
        "auth", "Tencent-Cloud.coding-copilot.info",
    )
    if os.path.isfile(exact_path):
        result = _parse_auth_file(exact_path)
        if result:
            return result

    pattern = os.path.join(
        local_app, "CodeBuddyExtension", "**",
        "Tencent-Cloud.coding-copilot.info",
    )
    for match in glob.glob(pattern, recursive=True):
        result = _parse_auth_file(match)
        if result:
            return result
    return None


def _parse_auth_file(path: str) -> dict | None:
    """解析 CodeBuddy 认证 JSON 文件。"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        account = data.get("account", {})
        auth = data.get("auth", {})
        token = auth.get("accessToken")
        uid = account.get("uid")
        if not token or not uid:
            return None
        return {
            "token": token,
            "user_id": uid,
            "enterprise_id": account.get("enterpriseId", ""),
            "department_info": account.get("departmentFullName", ""),
        }
    except (OSError, json.JSONDecodeError, TypeError):
        return None


def log(msg: str) -> None:
    """带时间戳的控制台日志。"""
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


class ProxyHandler(BaseHTTPRequestHandler):
    """将 OpenAI 兼容请求转发到 CodeBuddy 的 HTTP 处理器。"""

    def do_POST(self) -> None:
        if self.path not in ("/v1/chat/completions", "/chat/completions"):
            self.send_error(404, "Only /v1/chat/completions is supported")
            return

        auth = find_codebuddy_auth()
        if not auth:
            self.send_error(502, "CodeBuddy auth not found")
            log("ERROR: JWT file not found")
            return

        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length)

        try:
            body_obj = json.loads(body)
        except (json.JSONDecodeError, TypeError):
            self.send_error(400, "Invalid JSON")
            return

        client_wants_stream = body_obj.get("stream", True)
        model = body_obj.get("model", "?")

        # CodeBuddy 只支持流式，强制 stream=true
        body_obj["stream"] = True
        forwarded_body = json.dumps(body_obj).encode("utf-8")

        headers = {
            "Authorization": f"Bearer {auth['token']}",
            "X-User-Id": auth["user_id"],
            "X-Enterprise-Id": auth["enterprise_id"],
            "X-Tenant-Id": auth["enterprise_id"],
            "X-Domain": "tencent.sso.copilot.tencent.com",
            "X-Department-Info": auth["department_info"],
            "X-Product": "SaaS",
            "X-Requested-With": "XMLHttpRequest",
            "X-Agent-Intent": "craft",
            "Content-Type": "application/json",
        }

        req = urllib.request.Request(
            CODEBUDDY_ENDPOINT, data=forwarded_body, headers=headers, method="POST",
        )

        try:
            ctx = ssl.create_default_context()
            resp = urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT, context=ctx)
        except urllib.error.HTTPError as exc:
            log(f"CodeBuddy HTTP {exc.code}: {exc.reason}")
            self.send_error(exc.code, f"CodeBuddy: {exc.reason}")
            return
        except Exception as exc:
            log(f"CodeBuddy request failed: {exc}")
            self.send_error(502, str(exc))
            return

        if client_wants_stream:
            self._stream_response(resp)
        else:
            self._aggregate_response(resp, model)

        log(f"OK: {model} -> CodeBuddy (stream={client_wants_stream})")

    def _stream_response(self, resp) -> None:
        """透传 SSE 流式响应给客户端。"""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        try:
            while True:
                chunk = resp.read(4096)
                if not chunk:
                    break
                hex_len = f"{len(chunk):X}\r\n".encode()
                self.wfile.write(hex_len + chunk + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            log("Client disconnected")

    def _aggregate_response(self, resp, model: str) -> None:
        """将 SSE 流聚合为标准 OpenAI 非流式响应。"""
        raw = resp.read().decode("utf-8", errors="replace")
        content_parts: list[str] = []
        chat_id = ""
        usage = None
        finish_reason = "stop"

        for line in raw.split("\n"):
            line = line.strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            try:
                chunk = json.loads(line[6:])
            except json.JSONDecodeError:
                continue
            if not chat_id:
                chat_id = chunk.get("id", "")
            if "usage" in chunk and chunk["usage"]:
                usage = chunk["usage"]
            for choice in chunk.get("choices", []):
                delta = choice.get("delta", {})
                if delta.get("content"):
                    content_parts.append(delta["content"])
                if choice.get("finish_reason"):
                    finish_reason = choice["finish_reason"]

        result = {
            "id": chat_id,
            "object": "chat.completion",
            "model": model,
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": "".join(content_parts)},
                "finish_reason": finish_reason,
            }],
        }
        if usage:
            result["usage"] = usage

        body = json.dumps(result).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/v1/models":
            models_response = {
                "object": "list",
                "data": [
                    {"id": "glm-5.0", "object": "model", "owned_by": "codebuddy"},
                    {"id": "glm-4.9", "object": "model", "owned_by": "codebuddy"},
                ],
            }
            body = json.dumps(models_response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/health":
            auth = find_codebuddy_auth()
            status = "ok" if auth else "no_auth"
            body = json.dumps({"status": status}).encode("utf-8")
            self.send_response(200 if auth else 503)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_error(404)

    def do_OPTIONS(self) -> None:
        """处理 CORS 预检请求。"""
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
        self.end_headers()

    def log_message(self, format: str, *args: object) -> None:
        """静默 BaseHTTPRequestHandler 默认的 stderr 日志。"""
        pass


def main() -> None:
    port = DEFAULT_PORT
    if len(sys.argv) > 1 and sys.argv[1] == "--port":
        port = int(sys.argv[2])

    auth = find_codebuddy_auth()
    if auth:
        log(f"JWT found, user={auth['user_id']}")
    else:
        log("WARNING: JWT not found, requests will fail until CodeBuddy logs in")

    server = HTTPServer(("127.0.0.1", port), ProxyHandler)
    log(f"CodeBuddy proxy listening on http://localhost:{port}/v1")
    log("Cursor config: Override OpenAI Base URL -> http://localhost:{}/v1".format(port))

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Shutting down")
        server.server_close()


if __name__ == "__main__":
    main()
