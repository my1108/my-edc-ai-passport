#!/usr/bin/env python3
"""Link the AI Passport home screen to a Spotify account.

Runs the Spotify Authorization Code flow with PKCE once on a computer, then
prints the two serial-console commands that store the result on the device:

    spotify client <client ID>
    spotify token <refresh token>

Only the Python standard library is used. Nothing is written to disk. The
refresh token is a password for your Spotify account: paste it only into the
device's serial console and do not commit, share, or log it.

Usage:
    python3 tools/spotify_auth.py --client-id <32-hex client ID>

In the Spotify developer dashboard the app must list this exact redirect URI
(Spotify does not accept "localhost"; use the loopback IP):
    http://127.0.0.1:8888/callback
"""

import argparse
import base64
import hashlib
import http.server
import json
import re
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

AUTHORIZE_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
PLAYER_URL = "https://api.spotify.com/v1/me/player"
SCOPES = "user-read-playback-state user-modify-playback-state user-read-currently-playing"
TIMEOUT_S = 300


def make_verifier() -> str:
    # RFC 7636: 43-128 characters from the unreserved set.
    return secrets.token_urlsafe(64)[:96]


def make_challenge(verifier: str) -> str:
    digest = hashlib.sha256(verifier.encode("ascii")).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


def authorize_url(client_id: str, redirect_uri: str, challenge: str, state: str) -> str:
    query = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": redirect_uri,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
            "scope": SCOPES,
            "state": state,
        }
    )
    return f"{AUTHORIZE_URL}?{query}"


class _Callback(http.server.BaseHTTPRequestHandler):
    result = {}
    done = threading.Event()

    def do_GET(self):  # noqa: N802 (http.server API)
        url = urllib.parse.urlparse(self.path)
        if url.path != "/callback":
            self.send_error(404)
            return
        params = urllib.parse.parse_qs(url.query)
        _Callback.result = {k: v[0] for k, v in params.items()}
        ok = "code" in _Callback.result
        body = (
            "<h2>Spotify linked.</h2><p>You can close this tab and return to the terminal.</p>"
            if ok
            else "<h2>Spotify did not grant access.</h2><p>Return to the terminal for details.</p>"
        )
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(body.encode("utf-8"))
        _Callback.done.set()

    def log_message(self, fmt, *args):  # keep the code out of the terminal
        pass


def post_form(url: str, fields: dict) -> dict:
    data = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"}
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as err:
        try:
            detail = json.load(err)
        except ValueError:
            detail = {"error": f"HTTP {err.code}"}
        raise SystemExit(
            f"Token exchange failed: {detail.get('error')}: {detail.get('error_description', '')}"
        )


def check_player(access_token: str) -> str:
    req = urllib.request.Request(PLAYER_URL, headers={"Authorization": f"Bearer {access_token}"})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            if resp.status == 204:
                return "OK (nothing is playing right now)"
            state = json.load(resp)
            item = state.get("item") or {}
            return f"OK (now playing: {item.get('name', 'unknown')})"
    except urllib.error.HTTPError as err:
        if err.code == 403:
            return (
                "HTTP 403: check that this Spotify account is listed under User Management "
                "and that the app owner has Spotify Premium"
            )
        return f"HTTP {err.code}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client-id", help="Client ID from the Spotify developer dashboard")
    parser.add_argument("--port", type=int, default=8888, help="loopback port of the redirect URI")
    parser.add_argument("--no-browser", action="store_true", help="print the URL instead of opening it")
    args = parser.parse_args()

    client_id = (args.client_id or input("Spotify client ID: ")).strip()
    if not re.fullmatch(r"[0-9a-fA-F]{32}", client_id):
        print("The client ID must be the 32 hex digits shown in the developer dashboard.", file=sys.stderr)
        return 2

    redirect_uri = f"http://127.0.0.1:{args.port}/callback"
    verifier = make_verifier()
    state = secrets.token_urlsafe(16)
    url = authorize_url(client_id, redirect_uri, make_challenge(verifier), state)

    server = http.server.HTTPServer(("127.0.0.1", args.port), _Callback)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    print(f"Redirect URI (must match the dashboard exactly): {redirect_uri}")
    print("Sign in to Spotify and approve access in your browser.")
    if args.no_browser or not webbrowser.open(url):
        print(f"\nOpen this URL in a browser on this computer:\n{url}\n")

    if not _Callback.done.wait(TIMEOUT_S):
        server.shutdown()
        print("Timed out waiting for Spotify to redirect back.", file=sys.stderr)
        return 1
    server.shutdown()

    result = _Callback.result
    if result.get("state") != state:
        print("The reply did not match this request (state mismatch); try again.", file=sys.stderr)
        return 1
    if "code" not in result:
        print(f"Spotify returned an error: {result.get('error', 'unknown')}", file=sys.stderr)
        return 1

    tokens = post_form(
        TOKEN_URL,
        {
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": redirect_uri,
            "client_id": client_id,
            "code_verifier": verifier,
        },
    )
    refresh = tokens.get("refresh_token", "")
    if not re.fullmatch(r"[A-Za-z0-9._~-]{16,256}", refresh):
        print("Spotify did not return a usable refresh token.", file=sys.stderr)
        return 1

    granted = set(tokens.get("scope", "").split())
    missing = set(SCOPES.split()) - granted
    if missing:
        print(f"Warning: Spotify did not grant: {' '.join(sorted(missing))}", file=sys.stderr)
    print(f"Player check: {check_player(tokens.get('access_token', ''))}")

    print("\nPaste these two lines into the device's serial console (idf.py monitor):\n")
    print(f"spotify client {client_id}")
    print(f"spotify token {refresh}")
    print("\nThe refresh token is a secret. Do not commit, share, or log it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
