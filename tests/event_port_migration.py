"""Check that event-port migration can change only the two validated local callback ports."""

from __future__ import annotations

import io
import json
import pathlib
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import migrate_event_port as migration


def main() -> int:
    """Cover firmware whitespace, parameter preservation, mismatches and bounded rollback."""
    form = migration.Form()
    form.feed('<input name="AURL_CallEstablished_RW" value="\r\nhttp://192.0.2.20:8080/event?ev=est&amp;user=$active_user\r\n">')
    assert form.fields["AURL_CallEstablished_RW"] == "http://192.0.2.20:8080/event?ev=est&user=$active_user"
    original = "http://192.0.2.20:8080/event?ev=est&mac=$mac&line=$line&user=$active_user"
    assert migration.callback_url(original, "192.0.2.20", 8080, 18080, "est") == original.replace(":8080/", ":18080/")
    for changed in (original.replace("192.0.2.20", "192.0.2.30"), original.replace("8080", "9090"), original.replace("ev=est", "ev=end")):
        try:
            migration.callback_url(changed, "192.0.2.20", 8080, 18080, "est")
        except RuntimeError:
            pass
        else:
            raise AssertionError("unexpected callback scope accepted")

    class FakeClient:
        """Model a device with protected unrelated callbacks and an optional bad first readback."""
        address = "192.0.2.20"
        event_port = 18080

        def __init__(self, corrupt: bool):
            """Create synthetic form values, including a quote and Unicode in an unrelated field."""
            self.fields = {field: f"http://{self.address}:8080/event?ev={event}&user=$active_user" for field, event in migration.TARGETS.items()}
            self.fields.update({"AURL_Registered_RW": "https://example.test/status?token=synthetic", "Other": '测试 "value"', "ReturnPage": "/action.htm"})
            self.initial = dict(self.fields)
            self.corrupt = corrupt
            self.calls = 0

        def exchange(self, fields=None):
            """Apply a form or read it back; corruption occurs only after the first write."""
            self.calls += 1
            if fields is not None:
                self.fields = {key: value for key, value in fields.items() if key != "DefaultSubmit"}
                if self.corrupt:
                    self.fields["Other"] = "synthetic mismatch"
                    self.corrupt = False
            return dict(self.fields)

    health = {"event": True, "control": True, "call_active": False, "active_sessions": 0, "phone_health": 1}
    with tempfile.TemporaryDirectory(prefix="PhoneRecord-port-test-") as temporary:
        with patch.object(migration, "urlopen", side_effect=lambda *args, **kwargs: io.StringIO(json.dumps(health))), patch.object(migration, "protect", side_effect=lambda data: data), patch.object(migration.tempfile, "mkdtemp", return_value=temporary):
            good = FakeClient(False)
            result = migration.migrate(good, 8080, 18080, True)
            assert result["verified"] and result["other_fields_unchanged"] and good.calls == 3
            assert good.fields["Other"] == good.initial["Other"] and good.fields["AURL_Registered_RW"] == good.initial["AURL_Registered_RW"]
            bad = FakeClient(True)
            try:
                migration.migrate(bad, 8080, 18080, True)
            except RuntimeError:
                pass
            else:
                raise AssertionError("bad readback accepted")
            assert bad.fields == bad.initial and bad.calls == 5, "rollback was incomplete or queried once per field"
    print("PASS: firmware form normalization, exact callback port edits, unrelated-field preservation, scope refusal and bounded verified rollback")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
