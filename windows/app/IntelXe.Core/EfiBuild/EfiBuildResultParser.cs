// SPDX-License-Identifier: MIT
// The builder's result JSON (efibuild.py build --result FILE; DESIGN-efi-build.md section 3, "The result"): ok, error,
// policy, facts, decisions[], notices[], changes[], firmware[], after_install[], downloads[], acpi_tables[], upstream,
// opencore, checks {ok, failures[], notes[], checks {ocvalidate {ok, found, accepted[], issues[], error}}}. Read
// tolerantly: a missing part is empty, never an exception, so that a build that stopped early still shows its error.
using System.Text.Json;

namespace IntelXe.Core.EfiBuild;

public static class EfiBuildResultParser
{
    /// The result in json, with every string passed through redact (the SMBIOS values removed). Throws
    /// EfiBuildException when it is not the builder's result.
    public static EfiBuildResult Parse(string json, Func<string, string>? redact = null, string logPath = "", string resultPath = "", string efiDir = "")
    {
        redact ??= s => s;
        JsonDocument doc;
        try { doc = JsonDocument.Parse(json); }
        catch (JsonException e) { throw new EfiBuildException("the builder's result is not JSON: " + e.Message); }
        using (doc)
        {
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object || !r.TryGetProperty("ok", out var okEl) || okEl.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                throw new EfiBuildException("the builder's result has no \"ok\"");
            string S(JsonElement e, string name) => redact(Str(e, name));
            IReadOnlyList<string> L(JsonElement e, string name) => Strings(e, name).Select(redact).ToList();

            var policy = Obj(r, "policy");
            var facts = Obj(r, "facts");
            var checks = Obj(r, "checks");
            var oc = Obj(Obj(checks, "checks"), "ocvalidate");

            var decisions = Items(r, "decisions").Select(d => new EfiBuildDecision(S(d, "rule"), S(d, "screen"), S(d, "question"), S(d, "answer"), S(d, "why"))).ToList();
            var changes = Items(r, "changes").Select(c => new EfiBuildChange(S(c, "step"),
                Str(c, "change") is { Length: > 0 } ? S(c, "change") : S(c, "note"), S(c, "why"))).ToList();
            var ocv = oc.ValueKind == JsonValueKind.Object
                ? new OcValidateResult(Bool(oc, "ok"), Int(oc, "found"),
                    Items(oc, "accepted").Select(a => (S(a, "finding"), S(a, "why"))).ToList(), L(oc, "issues"),
                    Str(oc, "error") is { Length: > 0 } ? S(oc, "error") : null)
                : OcValidateResult.None;
            bool ok = okEl.GetBoolean();
            string? error = Str(r, "error") is { Length: > 0 } ? S(r, "error") : null;
            if (!ok && error is null) error = "the build did not finish";

            return new EfiBuildResult(ok, error, S(r, "opencore"), S(policy, "smbios"), S(policy, "macos"),
                decisions, changes, L(r, "firmware"), L(r, "after_install"), L(checks, "failures"), L(checks, "notes"),
                ocv, L(facts, "cards"), S(facts, "cpu"), logPath, resultPath, efiDir);
        }
    }

    static JsonElement Obj(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Object ? v : default;

    static IEnumerable<JsonElement> Items(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.Object).ToList()
            : [];

    static IEnumerable<string> Strings(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString() ?? "").ToList()
            : [];

    static string Str(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v)
            ? v.ValueKind switch { JsonValueKind.String => v.GetString() ?? "", JsonValueKind.Number => v.GetRawText(), _ => "" }
            : "";

    static bool Bool(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    static int Int(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n) ? n : 0;
}
