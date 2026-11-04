"""Upload an encrypted Actions signing secret; never print credentials or private key material."""

import base64
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request


def main():
    """Read the secret from stdin, seal it with the repository key, and verify secret metadata."""
    payload = json.load(sys.stdin)
    if payload.get("dependencyPath"):
        sys.path.insert(0, payload["dependencyPath"])
    try:
        from nacl.public import PublicKey, SealedBox
    except ImportError:
        raise SystemExit("Install PyNaCl for this one-time setup, or use gh secret set UPDATE_SIGNING_KEY.")
    environment = dict(os.environ, GCM_INTERACTIVE="never", GIT_TERMINAL_PROMPT="0")
    credential = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                                text=True, capture_output=True, env=environment, timeout=20)
    values = dict(line.split("=", 1) for line in credential.stdout.splitlines() if "=" in line)
    token = values.get("password")
    if credential.returncode or not token:
        raise SystemExit("Git Credential Manager has no usable GitHub login. Sign in before retrying.")
    root = "https://api.github.com/repos/ChenRanReal/PhoneRecord/actions/secrets"

    def request(suffix, data=None):
        """Call the fixed repository API without including sensitive response bodies in diagnostics."""
        headers = {"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
                   "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "PhoneRecord-publisher-setup"}
        raw = None if data is None else json.dumps(data).encode()
        req = urllib.request.Request(root + suffix, data=raw, headers=headers,
                                     method="GET" if data is None else "PUT")
        try:
            with urllib.request.urlopen(req, timeout=30) as response:
                body = response.read(65536)
                return json.loads(body) if body else {}
        except urllib.error.HTTPError as error:
            raise SystemExit(f"GitHub secret setup returned HTTP {error.code}; check repository and Actions-secret permissions.") from None

    key = request("/public-key")
    encrypted = SealedBox(PublicKey(base64.b64decode(key["key"]))).encrypt(payload.pop("privateKey").encode())
    request("/UPDATE_SIGNING_KEY", {"key_id": key["key_id"], "encrypted_value": base64.b64encode(encrypted).decode()})
    result = request("/UPDATE_SIGNING_KEY")
    print(json.dumps({"name": result["name"], "updated_at": result["updated_at"], "plaintext_logged": False}))


if __name__ == "__main__":
    main()
