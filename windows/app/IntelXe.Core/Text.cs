// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

/// A piece of text for the window: a key in the app's string tables (Strings/*/Resources.resw) with its arguments, or a
/// verbatim string (a device name as Windows reports it). Core never translates; the app resolves keys in the language
/// it shows. An argument may itself be a Text.
public readonly record struct Text(string? Key, string? Verbatim, object[] Args)
{
    public static Text Of(string key, params object[] args) => new(key, null, args);
    public static Text Raw(string text) => new(null, text, []);
    public static readonly Text Empty = new(null, "", []);
    public bool IsEmpty => Key is null && string.IsNullOrEmpty(Verbatim);
    public override string ToString() => Key is null ? Verbatim ?? "" : Key + (Args.Length > 0 ? "(" + string.Join(", ", Args) + ")" : "");
}
