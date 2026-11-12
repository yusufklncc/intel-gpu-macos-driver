# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
IntelXe-EFI.ps1: the OpenCore part of intel-gpu-macos-driver, done on Windows. It changes an OpenCore EFI folder that
you point it to (a USB stick's EFI folder, or the EFI partition mounted with mountvol): config.plist and files, with
a backup of the whole EFI folder first and an undo. The same changes as efi.sh in the macOS package.

.DESCRIPTION
Without arguments it opens a window. In a console:

  .\IntelXe-EFI.ps1 -Status -Efi E:\EFI                  the EFI compared with the package, "key=value" lines
  .\IntelXe-EFI.ps1 -Plan settings -Efi E:\EFI           stage 2: SIP, AMFI and surfvram boot-args, quirks
  .\IntelXe-EFI.ps1 -Apply settings -Efi E:\EFI -Expect HASH
  .\IntelXe-EFI.ps1 -Plan driver -Efi E:\EFI             stage 4: IntelXe.kext (and SSDT-ARCOFF.aml, see below;
                                                         HybridPM.kext on a hybrid CPU)
  .\IntelXe-EFI.ps1 -Apply driver -Efi E:\EFI -Expect HASH
  .\IntelXe-EFI.ps1 -Plan undo -Efi E:\EFI               what -Undo would put back
  .\IntelXe-EFI.ps1 -Undo -Efi E:\EFI -Expect HASH       one change back per run, from the newest backup
  .\IntelXe-EFI.ps1 -ListEsp                             the EFI partitions of this PC (administrator)
  .\IntelXe-EFI.ps1 -MountEsp '\\?\Volume{...}\' [-Letter S:]   mount one (administrator); -MountEsp system: the one
                                                         Windows booted from (mountvol S: /S)
  .\IntelXe-EFI.ps1 -UnmountEsp S:                       mountvol S: /D, for an EFI partition only

-Expect takes the SHA-256 that -Plan prints: nothing is written unless the plan is still that one.
-Efi: the EFI folder (the one holding OC and BOOT), or the folder or drive that holds it.
-Package: the unpacked intel-gpu-macos-driver-VERSION-efi.zip (default: this script's folder). Its SHA256SUMS is
checked before IntelXe.kext or SSDT-ARCOFF.aml is copied. Stage 2 does not need it.
-BackupDir: where the backups go (default: a folder IntelXe-EFI-backups next to the EFI folder; for an EFI partition
of less than 2 GB, %LOCALAPPDATA%\intel-gpu-macos-driver\efi-backups). Undo looks there too.
-CardAcpiPath: SSDT-ARCOFF.aml is added only with it, after macOS is installed and install.sh --apply has run:
the ACPI path of the B580's upstream port, e.g. \_SB.PC00.PEG1.PEGP (the shipped table's), or the "ACPI(...)" line of
Device Manager > the B580 > Properties > Details > Location paths. For another path than the shipped table's, the
table is adapted to it (the same bytes as efi.sh writes on macOS).
HybridPM.kext: on a 12th-14th gen Intel CPU with efficiency cores (by its name, Win32_Processor.Name; the CPU of the
PC this runs on), stage 4 also copies it and puts its entry right below Lilu.kext's, when the EFI loads Lilu.kext.
INTELXE_CPU_BRAND in the environment names another CPU (the tests).
-RulesFile: the EFI rules, efi-rules.json (default: the one next to this script, as in the -efi.zip; in the repository
release/templates/efi-rules.json). They say what each stage changes; efi.sh reads the same file on macOS.
#>
[CmdletBinding()]
param(
    [switch]$Status,
    [ValidateSet('settings', 'driver', 'undo')][string]$Plan,
    [ValidateSet('settings', 'driver')][string]$Apply,
    [switch]$Undo,
    [string]$Efi,
    [string]$Package,
    [string]$BackupDir,
    [string]$CardAcpiPath,
    [string]$Expect,
    [switch]$ListEsp,
    [string]$MountEsp,
    [string]$Letter,
    [string]$UnmountEsp,
    [string]$RulesFile,
    [switch]$Gui,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 1

# ---- the engine: config.plist, plans, backups, undo ------------------------------------------------------------------
# C# 5, so that Windows PowerShell 5.1 (.NET Framework's compiler) and PowerShell 7 both compile it. On macOS,
# windows/test/make-expected.sh builds this same text with dotnet and checks it against efi.sh.
$EngineSource = @'
// BEGIN ENGINE
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace IntelXeEfi
{
    public class EfiException : Exception
    {
        public EfiException(string message) : base(message) { }
    }

    public static class Util
    {
        public static string Hex(byte[] bytes)
        {
            var sb = new StringBuilder(bytes.Length * 2);
            foreach (byte b in bytes) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
        public static string Sha256(byte[] bytes)
        {
            using (var h = SHA256.Create()) return Hex(h.ComputeHash(bytes));
        }
        public static string Sha256Text(string text) { return Sha256(new UTF8Encoding(false).GetBytes(text)); }
        public static string Sha256File(string path)
        {
            using (var h = SHA256.Create())
            using (var f = File.OpenRead(path)) return Hex(h.ComputeHash(f));
        }
        public static bool SameFile(string a, string b)
        {
            if (!File.Exists(a) || !File.Exists(b)) return false;
            if (new FileInfo(a).Length != new FileInfo(b).Length) return false;
            return SameBytes(File.ReadAllBytes(a), File.ReadAllBytes(b));
        }
        public static bool SameBytes(byte[] x, byte[] y)
        {
            if (x == null || y == null || x.Length != y.Length) return false;
            for (int i = 0; i < x.Length; i++) if (x[i] != y[i]) return false;
            return true;
        }
        public static bool Contains(byte[] hay, byte[] needle) { return Find(hay, needle, 0) >= 0; }
        /// Where needle first starts in hay at or after from, or -1.
        public static int Find(byte[] hay, byte[] needle, int from)
        {
            for (int i = Math.Max(from, 0); i + needle.Length <= hay.Length; i++)
            {
                int j = 0;
                while (j < needle.Length && hay[i + j] == needle[j]) j++;
                if (j == needle.Length) return i;
            }
            return -1;
        }
        public static bool Exists(string p) { return File.Exists(p) || Directory.Exists(p); }
        public static string Native(string rel) { return rel.Replace('/', Path.DirectorySeparatorChar); }
        public static void WriteText(string path, string text) { File.WriteAllText(path, text, new UTF8Encoding(false)); }
        public static string ReadLine1(string path, string otherwise)
        {
            if (!File.Exists(path)) return otherwise;
            return File.ReadAllText(path).TrimEnd('\r', '\n');
        }

        static void Writable(string path)
        {
            var info = new FileInfo(path);
            if ((info.Attributes & FileAttributes.ReadOnly) != 0) info.Attributes &= ~FileAttributes.ReadOnly;
        }
        /// A file or a folder with everything in it, read-only files included; nothing when it is not there.
        public static void Delete(string path)
        {
            if (Directory.Exists(path))
            {
                foreach (string f in Directory.GetFiles(path, "*", SearchOption.AllDirectories)) Writable(f);
                Directory.Delete(path, true);
            }
            else if (File.Exists(path))
            {
                Writable(path);
                File.Delete(path);
            }
        }
        /// A copy of a folder (hidden and system files too) or of a file.
        public static void CopyTree(string src, string dst)
        {
            if (File.Exists(src))
            {
                string parent = Path.GetDirectoryName(dst);
                if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
                File.Copy(src, dst, false);
                return;
            }
            Directory.CreateDirectory(dst);
            foreach (string f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), false);
            foreach (string d in Directory.GetDirectories(src)) CopyTree(d, Path.Combine(dst, Path.GetFileName(d)));
        }
        /// dst replaced by a copy of src: copied next to it first, then the old one removed and the copy renamed.
        public static void PutCopy(string src, string dst)
        {
            string tmp = dst + ".new";
            Delete(tmp);
            CopyTree(src, tmp);
            Delete(dst);
            if (Directory.Exists(tmp)) Directory.Move(tmp, dst); else File.Move(tmp, dst);
        }
        /// dst replaced by a file with these bytes: written next to it first, read back, then put in its place.
        public static void PutBytes(byte[] bytes, string dst)
        {
            string tmp = dst + ".new";
            string parent = Path.GetDirectoryName(dst);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            Delete(tmp);
            File.WriteAllBytes(tmp, bytes);
            if (!SameBytes(File.ReadAllBytes(tmp), bytes)) throw new EfiException("writing " + tmp + " failed: it reads back differently");
            Delete(dst);
            File.Move(tmp, dst);
        }
        /// A file's new content, written next to it first and then put in its place.
        public static void Replace(string path, byte[] bytes)
        {
            string tmp = path + ".new";
            Delete(tmp);
            File.WriteAllBytes(tmp, bytes);
            try { File.Replace(tmp, path, null); }
            catch (Exception)
            {
                // ReplaceFile is not available everywhere (some FAT and network volumes): a plain copy instead
                if (File.Exists(tmp)) { File.Copy(tmp, path, true); File.Delete(tmp); }
            }
            byte[] back = File.ReadAllBytes(path);
            if (Sha256(back) != Sha256(bytes)) throw new EfiException("writing " + path + " failed: it reads back differently");
        }
        public static long TreeSize(string dir, out int files)
        {
            long size = 0;
            files = 0;
            if (File.Exists(dir)) { files = 1; return new FileInfo(dir).Length; }
            if (!Directory.Exists(dir)) return 0;
            foreach (string f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories)) { size += new FileInfo(f).Length; files++; }
            return size;
        }
        public static long FreeSpace(string path)
        {
            try
            {
                string root = Path.GetPathRoot(Path.GetFullPath(path));
                if (string.IsNullOrEmpty(root) || root.StartsWith(@"\\")) return -1;
                return new DriveInfo(root).AvailableFreeSpace;
            }
            catch (Exception) { return -1; }
        }
        /// Is child the folder parent, or inside it?
        public static bool Inside(string child, string parent)
        {
            var cmp = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            string c = Path.GetFullPath(child).TrimEnd('\\', '/'), p = Path.GetFullPath(parent).TrimEnd('\\', '/');
            return c.Equals(p, cmp) || c.StartsWith(p + Path.DirectorySeparatorChar, cmp);
        }
        public static string Size(long bytes)
        {
            if (bytes >= 1000L * 1000 * 1000) return Math.Round(bytes / 1e9).ToString() + " GB";
            return Math.Round(bytes / 1e6).ToString() + " MB";
        }
        /// For the tests: how many lines of a and b lie outside their longest common sequence of lines.
        public static int ChangedLines(string a, string b)
        {
            string[] x = a.Replace("\r\n", "\n").Split('\n'), y = b.Replace("\r\n", "\n").Split('\n');
            var l = new int[x.Length + 1, y.Length + 1];
            for (int i = x.Length - 1; i >= 0; i--)
                for (int j = y.Length - 1; j >= 0; j--)
                    l[i, j] = x[i] == y[j] ? l[i + 1, j + 1] + 1 : Math.Max(l[i + 1, j], l[i, j + 1]);
            return x.Length + y.Length - 2 * l[0, 0];
        }
    }

    /// A value to put into config.plist.
    public sealed class PValue
    {
        public string Type;   // string, integer, bool, data, dict, array
        public string Text = "";
        public List<KeyValuePair<string, PValue>> Entries = new List<KeyValuePair<string, PValue>>();
        public static PValue Str(string s) { var v = new PValue(); v.Type = "string"; v.Text = s; return v; }
        public static PValue Int(long n) { var v = new PValue(); v.Type = "integer"; v.Text = n.ToString(); return v; }
        public static PValue Bool(bool b) { var v = new PValue(); v.Type = "bool"; v.Text = b ? "true" : "false"; return v; }
        public static PValue Data(byte[] bytes) { var v = new PValue(); v.Type = "data"; v.Text = Convert.ToBase64String(bytes); return v; }
        public static PValue Array() { var v = new PValue(); v.Type = "array"; return v; }
        /// Dict("key", value, "key", value, ...): a value is a PValue, a string, a bool or a long.
        public static PValue Dict(params object[] keysAndValues)
        {
            var v = new PValue();
            v.Type = "dict";
            for (int i = 0; i + 1 < keysAndValues.Length; i += 2)
            {
                object o = keysAndValues[i + 1];
                PValue x = o as PValue;
                if (x == null && o is string) x = Str((string)o);
                if (x == null && o is bool) x = Bool((bool)o);
                if (x == null && o is long) x = Int((long)o);
                if (x == null && o is int) x = Int((int)o);
                v.Entries.Add(new KeyValuePair<string, PValue>((string)keysAndValues[i], x));
            }
            return v;
        }
        public string ElementName { get { return Type == "bool" ? Text : Type; } }
        /// What Plist.Raw gives for it once written.
        public string Raw { get { return Type == "dict" || Type == "array" ? Entries.Count.ToString() : Text; } }
    }

    /// One step of a plan: copy (Source, or Bytes when set, -> EFI/OC/Rel), set, append, insert (at Index of the array
    /// at Path), mkdict, mkarray (Path in config.plist).
    public sealed class Op
    {
        public string Kind;
        public string[] Path;
        public int Index;
        public PValue Value;
        public string Source;
        public byte[] Bytes;
        public string Rel;
        public static Op Copy(string source, string rel) { var o = new Op(); o.Kind = "copy"; o.Source = source; o.Rel = rel; return o; }
        public static Op CopyBytes(byte[] bytes, string rel) { var o = new Op(); o.Kind = "copy"; o.Bytes = bytes; o.Rel = rel; return o; }
        public static Op Set(string[] path, PValue v) { var o = new Op(); o.Kind = "set"; o.Path = path; o.Value = v; return o; }
        public static Op Append(string[] path, PValue v) { var o = new Op(); o.Kind = "append"; o.Path = path; o.Value = v; return o; }
        public static Op Insert(string[] path, int index, PValue v) { var o = new Op(); o.Kind = "insert"; o.Path = path; o.Index = index; o.Value = v; return o; }
        public static Op Make(string kind, string[] path)
        {
            var o = new Op(); o.Kind = kind; o.Path = path; o.Value = kind == "mkdict" ? PValue.Dict() : PValue.Array(); return o;
        }
        public static Op Restore(string rel) { var o = new Op(); o.Kind = "restore"; o.Rel = rel; return o; }
    }

    /// A plan: the lines shown, its notes, and what applying it does. Its hash is the SHA-256 of Text.
    public sealed class Plan
    {
        public string Kind;
        public string Where;
        public List<string> Lines = new List<string>();
        public List<string> Notes = new List<string>();
        public List<Op> Ops = new List<Op>();
        public string Text
        {
            get
            {
                var sb = new StringBuilder();
                sb.Append("EFI: ").Append(Where).Append('\n');
                if (Lines.Count == 0) sb.Append("nothing to change\n");
                foreach (string l in Lines) sb.Append(l).Append('\n');
                foreach (string n in Notes) sb.Append("note     ").Append(n).Append('\n');
                return sb.ToString();
            }
        }
        public string Hash { get { return Util.Sha256Text(Text); } }
        public bool HasChanges { get { return Ops.Count > 0; } }
    }

    /// config.plist as XML. Written back with its own layout: only the values that change and the entries that are
    /// added differ; key order, comments, the DOCTYPE, indentation and line ends stay.
    public sealed class Plist
    {
        public XmlDocument Doc;
        bool bom, crlf, finalNewline;
        string unit = "\t";

        public static Plist Load(string path) { return Parse(File.ReadAllBytes(path), path); }

        public static Plist Parse(byte[] bytes, string name)
        {
            var p = new Plist();
            int start = 0;
            if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) { p.bom = true; start = 3; }
            if (bytes.Length - start >= 6 && Encoding.ASCII.GetString(bytes, start, 6) == "bplist")
                throw new EfiException(name + " is a binary property list; OpenCore reads XML only");
            string text;
            try { text = new UTF8Encoding(false, true).GetString(bytes, start, bytes.Length - start); }
            catch (Exception) { throw new EfiException(name + " is not UTF-8 text"); }
            p.crlf = text.Contains("\r\n");
            p.finalNewline = text.EndsWith("\n");
            Match m = Regex.Match(text, "\n([ \t]+)<");
            if (m.Success) p.unit = m.Groups[1].Value;
            var settings = new XmlReaderSettings();
            settings.DtdProcessing = DtdProcessing.Parse;   // keeps the DOCTYPE; the DTD itself is never fetched
            settings.XmlResolver = null;
            settings.IgnoreWhitespace = false;
            settings.IgnoreComments = false;
            var doc = new XmlDocument();
            doc.PreserveWhitespace = true;
            doc.XmlResolver = null;
            try
            {
                using (var sr = new StringReader(text))
                using (var xr = XmlReader.Create(sr, settings)) doc.Load(xr);
            }
            catch (XmlException e) { throw new EfiException(name + " is not a valid property list: " + e.Message); }
            p.Doc = doc;
            if (doc.DocumentElement == null || doc.DocumentElement.Name != "plist" || p.Root == null)
                throw new EfiException(name + " is not a property list with a dictionary at its top");
            return p;
        }

        public XmlElement Root
        {
            get
            {
                List<XmlElement> top = Elements(Doc.DocumentElement);
                return top.Count > 0 && top[0].Name == "dict" ? top[0] : null;
            }
        }

        public static List<XmlElement> Elements(XmlNode n)
        {
            var list = new List<XmlElement>();
            if (n == null) return list;
            foreach (XmlNode c in n.ChildNodes) if (c.NodeType == XmlNodeType.Element) list.Add((XmlElement)c);
            return list;
        }

        public static XmlElement DictValue(XmlElement dict, string key)
        {
            if (dict == null || dict.Name != "dict") return null;
            List<XmlElement> kids = Elements(dict);
            for (int i = 0; i + 1 < kids.Count; i += 2)
                if (kids[i].Name == "key" && kids[i].InnerText == key) return kids[i + 1];
            return null;
        }

        /// The element at a path of keys (and array indexes), or null.
        public XmlElement Get(params string[] path)
        {
            XmlElement cur = Root;
            foreach (string seg in path)
            {
                if (cur == null) return null;
                if (cur.Name == "dict") cur = DictValue(cur, seg);
                else if (cur.Name == "array")
                {
                    int i;
                    List<XmlElement> items = Elements(cur);
                    cur = int.TryParse(seg, out i) && i >= 0 && i < items.Count ? items[i] : null;
                }
                else return null;
            }
            return cur;
        }

        /// A value as "plutil -extract KEY raw" prints it: true/false, the number, the text, data as base64, the
        /// number of entries of a dict or an array; "" when there is none.
        public static string Raw(XmlElement e)
        {
            if (e == null) return "";
            switch (e.Name)
            {
                case "true": return "true";
                case "false": return "false";
                case "integer": case "real": case "date": return e.InnerText.Trim();
                case "data":
                    byte[] b = DataBytes(e);
                    return b == null ? Regex.Replace(e.InnerText, @"\s", "") : Convert.ToBase64String(b);
                case "dict": return (Elements(e).Count / 2).ToString();
                case "array": return Elements(e).Count.ToString();
                default: return e.InnerText;
            }
        }

        public static byte[] DataBytes(XmlElement e)
        {
            if (e == null || e.Name != "data") return null;
            try { return Convert.FromBase64String(Regex.Replace(e.InnerText, @"\s", "")); }
            catch (FormatException) { return null; }
        }

        static string[] Prefix(string[] path)
        {
            var p = new string[path.Length - 1];
            System.Array.Copy(path, p, p.Length);
            return p;
        }
        public static string Show(string[] path) { return string.Join(" > ", path); }

        static bool IsSpace(XmlNode n)
        {
            return n != null && (n.NodeType == XmlNodeType.Whitespace || n.NodeType == XmlNodeType.SignificantWhitespace);
        }
        /// The indentation before an element: the whitespace after the last line break before it.
        static string IndentOf(XmlNode e)
        {
            XmlNode p = e.PreviousSibling;
            if (IsSpace(p))
            {
                int i = p.Value.LastIndexOf('\n');
                if (i >= 0) return p.Value.Substring(i + 1);
            }
            return null;
        }
        string ChildIndent(XmlElement container)
        {
            List<XmlElement> kids = Elements(container);
            if (kids.Count > 0)
            {
                string first = IndentOf(kids[0]);
                if (first != null) return first;
            }
            return (IndentOf(container) ?? "") + unit;
        }
        /// Nodes added at the end of a dict or an array, each on its own line, before the closing tag.
        void AddAtEnd(XmlElement container, string indent, params XmlNode[] nodes)
        {
            XmlNode trailing = container.LastChild;
            if (!IsSpace(trailing))
            {
                trailing = Doc.CreateWhitespace("\n" + (IndentOf(container) ?? ""));
                container.AppendChild(trailing);
            }
            foreach (XmlNode n in nodes)
            {
                container.InsertBefore(Doc.CreateWhitespace("\n" + indent), trailing);
                container.InsertBefore(n, trailing);
            }
        }

        XmlElement Build(PValue v, string indent)
        {
            XmlElement e = Doc.CreateElement(v.ElementName);
            switch (v.Type)
            {
                case "bool": break;
                case "dict":
                    foreach (KeyValuePair<string, PValue> kv in v.Entries)
                    {
                        XmlElement k = Doc.CreateElement("key");
                        k.AppendChild(Doc.CreateTextNode(kv.Key));
                        AddAtEnd(e, indent + unit, k, Build(kv.Value, indent + unit));
                    }
                    if (v.Entries.Count > 0) ((XmlWhitespace)e.LastChild).Value = "\n" + indent;
                    break;
                case "array": break;
                default:
                    if (v.Text.Length > 0) e.AppendChild(Doc.CreateTextNode(v.Text)); else e.IsEmpty = false;
                    break;
            }
            return e;
        }

        void SetText(XmlElement e, PValue v)
        {
            string text = v.Text;
            if (v.Type == "data")
            {
                // keep the layout of the old value, e.g. plutil's "<data>\n\t\t\t\tAwgAAA==\n\t\t\t\t</data>"
                string old = e.InnerText;
                if (old.Trim().Length > 0)
                    text = Regex.Match(old, @"^\s*").Value + text + Regex.Match(old, @"\s*$").Value;
            }
            while (e.HasChildNodes) e.RemoveChild(e.FirstChild);
            if (text.Length > 0) e.AppendChild(Doc.CreateTextNode(text)); else e.IsEmpty = false;
        }

        void ReplaceValue(XmlElement old, PValue v)
        {
            if (old.Name == v.ElementName)
            {
                if (v.Type == "bool") return;
                if (v.Type == "string" || v.Type == "integer" || v.Type == "data") { SetText(old, v); return; }
            }
            old.ParentNode.ReplaceChild(Build(v, IndentOf(old) ?? ""), old);
        }

        /// path's value set to v; the key is added at the end of its dict when it is not there.
        public void Set(string[] path, PValue v)
        {
            XmlElement parent = Get(Prefix(path));
            string key = path[path.Length - 1];
            if (parent == null) throw new EfiException("config.plist has no " + Show(Prefix(path)));
            if (parent.Name == "dict")
            {
                XmlElement old = DictValue(parent, key);
                if (old != null) { ReplaceValue(old, v); return; }
                string indent = ChildIndent(parent);
                XmlElement k = Doc.CreateElement("key");
                k.AppendChild(Doc.CreateTextNode(key));
                AddAtEnd(parent, indent, k, Build(v, indent));
            }
            else if (parent.Name == "array")
            {
                XmlElement old = Get(path);
                if (old == null) throw new EfiException("config.plist has no " + Show(path));
                ReplaceValue(old, v);
            }
            else throw new EfiException("config.plist: " + Show(Prefix(path)) + " is not a dictionary");
        }

        /// v added at the end of the array at path (made when its dict has no such key).
        public void Append(string[] path, PValue v)
        {
            XmlElement arr = Get(path);
            if (arr == null) { Set(path, PValue.Array()); arr = Get(path); }
            if (arr.Name != "array") throw new EfiException("config.plist: " + Show(path) + " is not an array");
            string indent = ChildIndent(arr);
            AddAtEnd(arr, indent, Build(v, indent));
        }

        /// v inserted as item index of the array at path (the items from index on move up by one), on its own line
        /// before that item; at the end when index is the array's length.
        public void Insert(string[] path, int index, PValue v)
        {
            XmlElement arr = Get(path);
            if (arr == null || arr.Name != "array") throw new EfiException("config.plist: " + Show(path) + " is not an array");
            List<XmlElement> items = Elements(arr);
            if (index < 0 || index > items.Count) throw new EfiException("config.plist: " + Show(path) + " has no item " + index);
            if (index == items.Count) { Append(path, v); return; }
            string indent = ChildIndent(arr);
            XmlElement next = items[index];
            arr.InsertBefore(Build(v, indent), next);
            arr.InsertBefore(Doc.CreateWhitespace("\n" + indent), next);
        }

        public void Apply(Op op)
        {
            switch (op.Kind)
            {
                case "set": Set(op.Path, op.Value); break;
                case "append": Append(op.Path, op.Value); break;
                case "insert": Insert(op.Path, op.Index, op.Value); break;
                case "mkdict": case "mkarray": if (Get(op.Path) == null) Set(op.Path, op.Value); break;
                default: throw new EfiException("not a config.plist step: " + op.Kind);
            }
        }

        // ---- writing --------------------------------------------------------------------------------------------
        static string Escape(string s, bool attribute)
        {
            s = s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");
            return attribute ? s.Replace("\"", "&quot;") : s;
        }

        static void Write(StringBuilder sb, XmlNode n)
        {
            switch (n.NodeType)
            {
                case XmlNodeType.XmlDeclaration:
                    var d = (XmlDeclaration)n;
                    sb.Append("<?xml version=\"").Append(d.Version).Append('"');
                    if (!string.IsNullOrEmpty(d.Encoding)) sb.Append(" encoding=\"").Append(d.Encoding).Append('"');
                    if (!string.IsNullOrEmpty(d.Standalone)) sb.Append(" standalone=\"").Append(d.Standalone).Append('"');
                    sb.Append("?>");
                    break;
                case XmlNodeType.DocumentType:
                    var t = (XmlDocumentType)n;
                    sb.Append("<!DOCTYPE ").Append(t.Name);
                    if (!string.IsNullOrEmpty(t.PublicId)) sb.Append(" PUBLIC \"").Append(t.PublicId).Append("\" \"").Append(t.SystemId ?? "").Append('"');
                    else if (!string.IsNullOrEmpty(t.SystemId)) sb.Append(" SYSTEM \"").Append(t.SystemId).Append('"');
                    if (!string.IsNullOrEmpty(t.InternalSubset)) sb.Append(" [").Append(t.InternalSubset).Append(']');
                    sb.Append('>');
                    break;
                case XmlNodeType.Element:
                    var e = (XmlElement)n;
                    sb.Append('<').Append(e.Name);
                    foreach (XmlAttribute a in e.Attributes) sb.Append(' ').Append(a.Name).Append("=\"").Append(Escape(a.Value, true)).Append('"');
                    if (!e.HasChildNodes)
                    {
                        if (e.IsEmpty) sb.Append("/>"); else sb.Append("></").Append(e.Name).Append('>');
                    }
                    else
                    {
                        sb.Append('>');
                        foreach (XmlNode c in e.ChildNodes) Write(sb, c);
                        sb.Append("</").Append(e.Name).Append('>');
                    }
                    break;
                case XmlNodeType.Text: sb.Append(Escape(n.Value, false)); break;
                case XmlNodeType.Whitespace: case XmlNodeType.SignificantWhitespace: sb.Append(n.Value); break;
                case XmlNodeType.CDATA: sb.Append("<![CDATA[").Append(n.Value).Append("]]>"); break;
                case XmlNodeType.Comment: sb.Append("<!--").Append(n.Value).Append("-->"); break;
                case XmlNodeType.ProcessingInstruction:
                    sb.Append("<?").Append(n.Name);
                    if (!string.IsNullOrEmpty(n.Value)) sb.Append(' ').Append(n.Value);
                    sb.Append("?>");
                    break;
                case XmlNodeType.EntityReference: sb.Append('&').Append(n.Name).Append(';'); break;
                default: throw new EfiException("config.plist: cannot write an XML node of type " + n.NodeType);
            }
        }

        public string Serialize()
        {
            var sb = new StringBuilder();
            XmlNode prev = null;
            foreach (XmlNode n in Doc.ChildNodes)
            {
                if (prev != null && !IsSpace(prev) && !IsSpace(n)) sb.Append('\n');
                Write(sb, n);
                prev = n;
            }
            string s = sb.ToString();
            if (finalNewline && !s.EndsWith("\n")) s += "\n";
            if (crlf) s = s.Replace("\r\n", "\n").Replace("\n", "\r\n");
            return s;
        }

        public byte[] ToBytes()
        {
            byte[] body = new UTF8Encoding(false).GetBytes(Serialize());
            if (!bom) return body;
            var all = new byte[body.Length + 3];
            all[0] = 0xEF; all[1] = 0xBB; all[2] = 0xBF;
            System.Array.Copy(body, 0, all, 3, body.Length);
            return all;
        }

        // ---- comparing (the tests) ------------------------------------------------------------------------------
        /// The parsed content as one string: dict keys sorted, data as hex, numbers as numbers. Two property lists
        /// with the same keys and values give the same string, whatever their layout and key order.
        public static string Canonical(XmlElement e)
        {
            if (e == null) return "-";
            switch (e.Name)
            {
                case "dict":
                    var pairs = new SortedDictionary<string, string>(StringComparer.Ordinal);
                    List<XmlElement> kids = Elements(e);
                    for (int i = 0; i + 1 < kids.Count; i += 2) pairs[kids[i].InnerText] = Canonical(kids[i + 1]);
                    var sb = new StringBuilder("{");
                    foreach (KeyValuePair<string, string> kv in pairs) sb.Append(Quote(kv.Key)).Append('=').Append(kv.Value).Append(';');
                    return sb.Append('}').ToString();
                case "array":
                    var ab = new StringBuilder("[");
                    foreach (XmlElement c in Elements(e)) ab.Append(Canonical(c)).Append(',');
                    return ab.Append(']').ToString();
                case "true": return "b1";
                case "false": return "b0";
                case "integer":
                    long n;
                    return long.TryParse(e.InnerText.Trim(), out n) ? "i" + n : "i?" + Quote(e.InnerText.Trim());
                case "data":
                    byte[] b = DataBytes(e);
                    return b == null ? "d?" + Quote(e.InnerText) : "d" + Util.Hex(b);
                case "string": return "s" + Quote(e.InnerText);
                default: return e.Name + Quote(e.InnerText.Trim());
            }
        }
        static string Quote(string s) { return "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\""; }

        /// The first place where two property lists differ, or null when they hold the same.
        public static string Difference(XmlElement a, XmlElement b, string at)
        {
            if (Canonical(a) == Canonical(b)) return null;
            if (a != null && b != null && a.Name == "dict" && b.Name == "dict")
            {
                var keys = new SortedDictionary<string, bool>(StringComparer.Ordinal);
                foreach (XmlElement k in Elements(a)) if (k.Name == "key") keys[k.InnerText] = true;
                foreach (XmlElement k in Elements(b)) if (k.Name == "key") keys[k.InnerText] = true;
                foreach (string k in keys.Keys)
                {
                    string d = Difference(DictValue(a, k), DictValue(b, k), at + " > " + k);
                    if (d != null) return d;
                }
            }
            if (a != null && b != null && a.Name == "array" && b.Name == "array" && Elements(a).Count == Elements(b).Count)
            {
                List<XmlElement> x = Elements(a), y = Elements(b);
                for (int i = 0; i < x.Count; i++)
                {
                    string d = Difference(x[i], y[i], at + " > " + i);
                    if (d != null) return d;
                }
            }
            string ca = Canonical(a), cb = Canonical(b);
            if (ca.Length > 120) ca = ca.Substring(0, 120) + "...";
            if (cb.Length > 120) cb = cb.Substring(0, 120) + "...";
            return at + ": " + ca + " / " + cb;
        }

        /// Every key path in the order of the file, for checking that an edit kept the order.
        public static List<string> KeyPaths(XmlElement e, string at, List<string> list)
        {
            if (list == null) list = new List<string>();
            if (e == null) return list;
            List<XmlElement> kids = Elements(e);
            if (e.Name == "dict")
                for (int i = 0; i + 1 < kids.Count; i += 2) { string p = at + "/" + kids[i].InnerText; list.Add(p); KeyPaths(kids[i + 1], p, list); }
            else if (e.Name == "array")
                for (int i = 0; i < kids.Count; i++) KeyPaths(kids[i], at + "/" + i, list);
            return list;
        }
        public static bool IsSubsequence(List<string> part, List<string> whole)
        {
            int j = 0;
            foreach (string s in whole) if (j < part.Count && part[j] == s) j++;
            return j == part.Count;
        }
    }

    /// SSDT-ARCOFF adapted to the card's port: adapt_ssdt in release/templates/efi.sh, step for step. The package's
    /// table (efi/SSDT-ARCOFF.dsl) names the port twice as an absolute NamePath: 5c, then for two NameSegs 2e, for
    /// more 2f and the count, then the 4-byte NameSegs padded with "_" (5c 2f 04 "_SB_PC00PEG1PEGP"). The table is
    ///   header (36 bytes; the length at 4, the checksum at 9)
    ///   If (a0) PkgLength Zero (00)  External (15) NamePath ObjectType ArgCount
    ///   If (a0) PkgLength  CondRefOf (\_OSI, Local0) && _OSI ("Darwin")
    ///       Scope (10) PkgLength NamePath  Device (BRG1) { ... }
    /// and ends with the Scope. Adapt finds those parts, builds the table again from them with the package's path and
    /// goes on only when that gives the package's table byte for byte; then it builds it with the card's path (a
    /// longer or shorter path changes the three PkgLengths and the length, which the build computes).
    public static class Aml
    {
        /// The NamePath of an absolute ACPI path (one that matches pattern, ssdt.port_pattern: \NAME(.NAME)*, each
        /// 1-4 of A-Z 0-9 _), or null for anything that is not one.
        public static byte[] NamePath(string path, string pattern)
        {
            if (path == null || path.IndexOf('\n') >= 0 || !Regex.IsMatch(path, pattern)) return null;
            string[] segs = path.Substring(1).Split('.');
            if (segs.Length > 255) return null;
            var b = new List<byte>();
            b.Add(0x5c);
            if (segs.Length == 2) b.Add(0x2e);
            else if (segs.Length > 2) { b.Add(0x2f); b.Add((byte)segs.Length); }
            foreach (string s in segs) b.AddRange(Encoding.ASCII.GetBytes(s.PadRight(4, '_')));
            return b.ToArray();
        }

        /// Do the table's bytes name the path? Its NamePath with "\" or without (before a Dual- or MultiNamePrefix), the
        /// prefix and the count included, so that a shorter path inside a longer one does not match.
        public static bool NamesPath(byte[] table, string path, string pattern)
        {
            byte[] np = NamePath(path, pattern);
            if (np == null || table == null) return false;
            if (np[1] == 0x2e || np[1] == 0x2f) np = Slice(np, 1, np.Length - 1);
            return Util.Contains(table, np);
        }

        static byte[] Slice(byte[] b, int start, int count)
        {
            var r = new byte[count];
            System.Array.Copy(b, start, r, 0, count);
            return r;
        }
        static bool At(byte[] h, byte[] x, int i)
        {
            if (i < 0 || i + x.Length > h.Length) return false;
            for (int j = 0; j < x.Length; j++) if (h[i + j] != x[j]) return false;
            return true;
        }
        static int Sum(byte[] b)
        {
            int s = 0;
            foreach (byte x in b) s += x;
            return s & 255;
        }

        /// The PkgLength at byte i: { its value, its size in bytes }, or null past the end.
        static int[] PkgLengthAt(byte[] b, int i)
        {
            if (i < 0 || i >= b.Length) return null;
            int lead = b[i], k = lead >> 6, v;
            if (i + k >= b.Length) return null;
            if (k == 0) v = lead & 63;
            else
            {
                v = lead & 15;
                for (int j = 1; j <= k; j++) v |= b[i + j] << (4 + 8 * (j - 1));
            }
            return new int[] { v, k + 1 };
        }

        /// The PkgLength for n bytes that follow it, in as few bytes as it fits (as iasl writes it); null when too long.
        static byte[] PkgLength(int n)
        {
            if (n + 1 <= 63) return new byte[] { (byte)(n + 1) };
            for (int k = 1; k <= 3; k++)
            {
                long v = n + 1 + k;
                if (v < (1L << (4 + 8 * k)))
                {
                    var r = new byte[k + 1];
                    r[0] = (byte)((k << 6) | (int)(v & 15));
                    for (int j = 0; j < k; j++) r[j + 1] = (byte)((v >> (4 + 8 * j)) & 255);
                    return r;
                }
            }
            return null;
        }

        static byte[] Wrap(byte opcode, List<byte> content)
        {
            byte[] pl = PkgLength(content.Count);
            if (pl == null) return null;
            var r = new List<byte>();
            r.Add(opcode);
            r.AddRange(pl);
            r.AddRange(content);
            return r.ToArray();
        }

        sealed class Parts
        {
            public byte[] Header, Axt, Pred, Body;
        }

        /// The table from the parts, naming np; null when a PkgLength does not fit.
        static byte[] Build(Parts p, byte[] np)
        {
            var ext = new List<byte>();
            ext.Add(0x00);
            ext.Add(0x15);
            ext.AddRange(np);
            ext.AddRange(p.Axt);
            var scope = new List<byte>(np);
            scope.AddRange(p.Body);
            byte[] extp = Wrap(0xa0, ext), scopep = Wrap(0x10, scope);
            if (extp == null || scopep == null) return null;
            var ifb = new List<byte>(p.Pred);
            ifb.AddRange(scopep);
            byte[] ifp = Wrap(0xa0, ifb);
            if (ifp == null) return null;
            int len = 36 + extp.Length + ifp.Length;
            var t = new byte[len];
            System.Array.Copy(p.Header, t, 36);
            for (int i = 0; i < 4; i++) t[4 + i] = (byte)((len >> (8 * i)) & 255);
            t[9] = 0;
            System.Array.Copy(extp, 0, t, 36, extp.Length);
            System.Array.Copy(ifp, 0, t, 36 + extp.Length, ifp.Length);
            t[9] = (byte)((256 - Sum(t)) & 255);
            return t;
        }

        /// The package's table h naming card instead of the shipped path, or null with why it cannot be.
        public static byte[] Adapt(byte[] h, string card, string shipped, string pattern, string tableName, out string why)
        {
            byte[] ec = NamePath(card, pattern), ep = NamePath(shipped, pattern);
            if (ec == null) { why = card + " is not an ACPI path this script can write into the table"; return null; }
            why = "the package's " + tableName + " is not laid out as SSDT-ARCOFF.dsl builds it";
            int n = h == null ? 0 : h.Length;
            if (n <= 40 || Encoding.ASCII.GetString(h, 0, 4) != "SSDT") return null;
            long hl = (long)h[4] | (long)h[5] << 8 | (long)h[6] << 16 | (long)h[7] << 24;
            if (hl != n || Sum(h) != 0) return null;
            // the port's NamePath twice: the External's right after the header, then the Scope's
            if (h[36] != 0xa0) return null;
            int[] pk = PkgLengthAt(h, 37);
            if (pk == null) return null;
            int first = 37 + pk[1];
            if (first + 1 >= n || h[first] != 0x00 || h[first + 1] != 0x15 || !At(h, ep, first + 2)) return null;
            int i = first + 2 + ep.Length;
            if (i + 1 >= n) return null;
            byte[] axt = { h[i], h[i + 1] };   // the External's ObjectType and ArgCount
            i += 2;
            if (37 + pk[0] != i) return null;   // the If (Zero) holds the External only
            if (Util.Find(h, ep, 0) != first + 2) return null;
            int q = Util.Find(h, ep, first + 2 + ep.Length);   // the byte where the Scope's NamePath starts
            if (q < 0 || Util.Find(h, ep, q + ep.Length) >= 0) return null;
            if (i >= n || h[i] != 0xa0) return null;
            pk = PkgLengthAt(h, i + 1);
            if (pk == null || i + 1 + pk[0] != n) return null;   // the If ends with the table
            int p1 = i + 1 + pk[1], sc = 0;   // the If's predicate starts at p1
            for (int k = 1; k <= 4; k++)       // the Scope opcode and its PkgLength before the NamePath
            {
                if (q - 1 - k < p1 || h[q - 1 - k] != 0x10 || (h[q - k] >> 6) != k - 1) continue;
                int[] s = PkgLengthAt(h, q - k);
                if (s != null && s[1] == k && q - k + s[0] == n) { sc = k; break; }   // the Scope ends with the table too
            }
            if (sc == 0) return null;
            var parts = new Parts();
            parts.Header = Slice(h, 0, 36);
            parts.Axt = axt;
            parts.Pred = Slice(h, p1, q - 1 - sc - p1);
            parts.Body = Slice(h, q + ep.Length, n - q - ep.Length);
            if (!Util.SameBytes(Build(parts, ep), h)) return null;   // the parts make the package's table again
            why = "the table built for " + card + " did not read back as built";
            byte[] t = Build(parts, ec);
            if (t == null || Sum(t) != 0 || !NamesPath(t, card, pattern)) return null;
            why = "";
            return t;
        }
    }

    /// The EFI rules, efi-rules.json (release/templates/; docs/design/DESIGN-efi-rules.md): the same file efi.sh reads on
    /// macOS. The script parses it (ConvertFrom-Json) and hands over a plain tree: dictionaries (IDictionary with string
    /// keys, in the file's order), lists (IList), strings, integers (int or long) and booleans. Keys are looked up by
    /// a dotted path, list items by their index ("settings.3.kind"); a missing or mistyped value is an error.
    public sealed class Rules
    {
        public const string Format = "intelxe-efi-rules";
        public const long FormatVersion = 1;
        public readonly object Tree;
        public readonly string Source;

        Rules(object tree, string source) { Tree = tree; Source = source; }

        public static Rules FromTree(object tree, string source)
        {
            var r = new Rules(tree, source);
            if (!(tree is IDictionary<string, object>)) throw new EfiException(source + " is not a JSON object");
            if (r.Get("format") as string != Format || !(r.Get("format_version") is long || r.Get("format_version") is int) || r.Int("format_version") != FormatVersion)
                throw new EfiException(source + " is " + r.Get("format") + " " + r.Get("format_version") + "; this tool reads " + Format + " " + FormatVersion);
            return r;
        }

        /// The value at a dotted path, or null.
        public object Get(string path)
        {
            object cur = Tree;
            foreach (string seg in path.Split('.'))
            {
                var d = cur as IDictionary<string, object>;
                var l = cur as System.Collections.IList;
                object next = null;
                int i;
                if (d != null) { if (!d.TryGetValue(seg, out next)) return null; }
                else if (l != null && int.TryParse(seg, out i) && i >= 0 && i < l.Count) next = l[i];
                else return null;
                cur = next;
            }
            return cur;
        }
        public bool Has(string path) { return Get(path) != null; }
        EfiException Missing(string path, string what) { return new EfiException(Source + " has no " + what + " at " + path); }
        public string Str(string path)
        {
            string s = Get(path) as string;
            if (s == null) throw Missing(path, "string");
            return s;
        }
        /// A string, or "" when there is none (an optional "why").
        public string StrOr(string path) { return Get(path) as string ?? ""; }
        public long Int(string path)
        {
            object o = Get(path);
            if (o is long) return (long)o;
            if (o is int) return (int)o;
            throw Missing(path, "integer");
        }
        /// "0x803" or a decimal integer.
        public long Number(string path)
        {
            object o = Get(path);
            string s = o as string;
            if (s != null && s.StartsWith("0x")) return Convert.ToInt64(s.Substring(2), 16);
            if (s != null) return long.Parse(s);
            return Int(path);
        }
        public System.Collections.IList List(string path)
        {
            var l = Get(path) as System.Collections.IList;
            if (l == null) throw Missing(path, "list");
            return l;
        }
        public List<string> Strings(string path)
        {
            var r = new List<string>();
            foreach (object o in List(path))
            {
                if (!(o is string)) throw Missing(path, "list of strings");
                r.Add((string)o);
            }
            return r;
        }
        public IDictionary<string, object> Dict(string path)
        {
            var d = Get(path) as IDictionary<string, object>;
            if (d == null) throw Missing(path, "object");
            return d;
        }
        /// A key path of config.plist ("NVRAM.Add.7C43...") as its keys.
        public string[] KeyPath(string path) { return Str(path).Split('.'); }

        /// A value of the rules as a config.plist value: string, integer, bool; an object as a dict of those.
        public static PValue Value(object o)
        {
            if (o is string) return PValue.Str((string)o);
            if (o is bool) return PValue.Bool((bool)o);
            if (o is long) return PValue.Int((long)o);
            if (o is int) return PValue.Int((int)o);
            var d = o as IDictionary<string, object>;
            if (d != null)
            {
                PValue v = PValue.Dict();
                foreach (KeyValuePair<string, object> kv in d) v.Entries.Add(new KeyValuePair<string, PValue>(kv.Key, Value(kv.Value)));
                return v;
            }
            throw new EfiException("efi-rules.json: a value that is not a string, an integer, a boolean or an object");
        }
        /// The same, of the type the rule names ("integer", "bool", "string").
        public PValue Value(string path, string type)
        {
            PValue v = Value(Get(path));
            if (v.Type != type) throw Missing(path, type);
            return v;
        }

        /// Does the PCI device vendor:device belong to the cards (cards.vendor_id, cards.device_ids)?
        public bool IsCard(string vendor, string device)
        {
            if (!string.Equals(vendor, Str("cards.vendor_id"), StringComparison.OrdinalIgnoreCase)) return false;
            foreach (string id in Strings("cards.device_ids")) if (string.Equals(id, device, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }
        /// A Windows device instance id, PCI\VEN_8086&DEV_E20B&SUBSYS_...: one of the cards?
        public bool IsCardInstance(string instanceId)
        {
            Match m = Regex.Match(instanceId ?? "", @"^PCI\\VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})", RegexOptions.IgnoreCase);
            return m.Success && IsCard(m.Groups[1].Value, m.Groups[2].Value);
        }
    }

    /// An OpenCore EFI folder and the package: what stage 2 and stage 4 change there, backups and undo, by the rules
    /// (efi-rules.json, as efi.sh on macOS); this class finds the files and applies them.
    public sealed class EfiFolder
    {
        static readonly string[] KernelAdd = { "Kernel", "Add" };
        static readonly string[] AcpiAdd = { "ACPI", "Add" };
        public readonly Rules R;
        public string KextName { get { return R.Str("kext.bundle"); } }
        public string SsdtName { get { return R.Str("ssdt.file"); } }
        public string HpmName { get { return R.Str("hybridpm.bundle"); } }
        public string LiluName { get { return R.Str("hybridpm.after"); } }
        public string ShippedPath { get { return R.Str("ssdt.shipped_port"); } }
        /// Does OpenCore record which EFI it booted from (its boot-path NVRAM variable)? Windows cannot tell: false, so
        /// stage 4 sets ExposeSensitiveData's bit as efi.sh does without a recorded boot path (the tests set it).
        public bool BootPathRecorded;

        public string EfiDir, RootDir, OcDir, ConfigPath;
        public bool OpenCore, Readable;
        public string ReadError = "";
        public string PackageDir = "", PackageKext = "", PackageSsdt = "", PackageHpm = "";
        public string PackageProblem;
        /// The CPU's name (Win32_Processor.Name): HybridPM.kext is added for a hybrid one ("" until it is set).
        public string Cpu = "";
        public string BackupRoot, BackupKey;
        /// The card's upstream port as the user confirmed it ("" until then; "none" when it has no ACPI name).
        public string Card = "";
        /// What Windows reports for the card on this PC, for the notes only ("" when there is no B580 here).
        public string CardDetected = "";

        public string BackupDir { get { return Path.Combine(BackupRoot, BackupKey); } }
        /// EFI/OC/REL of a rule's file: its folder and name ("kext.folder" + "kext.bundle": Kexts/IntelXe.kext).
        string Rel(string rule, string name) { return R.Str(rule + ".folder") + "/" + R.Str(rule + "." + name); }
        string KextRel { get { return Rel("kext", "bundle"); } }
        string SsdtRel { get { return Rel("ssdt", "file"); } }
        string HpmRel { get { return Rel("hybridpm", "bundle"); } }
        string EfiKext { get { return Path.Combine(OcDir, Util.Native(KextRel)); } }
        string EfiSsdt { get { return Path.Combine(OcDir, Util.Native(SsdtRel)); } }
        string EfiHpm { get { return Path.Combine(OcDir, Util.Native(HpmRel)); } }
        string EfiLilu { get { return Path.Combine(OcDir, Util.Native(R.Str("hybridpm.folder") + "/" + LiluName)); } }

        public EfiFolder(string efiPath, string packagePath, string backupRoot, Rules rules)
        {
            if (rules == null) throw new EfiException("no EFI rules (efi-rules.json)");
            R = rules;
            if (string.IsNullOrEmpty(efiPath) || efiPath.Trim().Length == 0) throw new EfiException("no EFI folder given");
            string full = Path.GetFullPath(efiPath.Trim().Trim('"'));
            if (!Directory.Exists(full)) throw new EfiException("the folder " + full + " does not exist");
            if (Directory.Exists(Path.Combine(full, "OC"))) EfiDir = full;
            else if (Directory.Exists(Path.Combine(Path.Combine(full, "EFI"), "OC"))) EfiDir = Path.Combine(full, "EFI");
            else throw new EfiException("no OC folder in " + full + ": choose the EFI folder of your OpenCore EFI (the one that holds OC and BOOT)");
            EfiDir = EfiDir.Length > 3 ? EfiDir.TrimEnd('\\', '/') : EfiDir;
            DirectoryInfo parent = Directory.GetParent(EfiDir);
            RootDir = parent == null ? EfiDir : parent.FullName;
            OcDir = Path.Combine(EfiDir, "OC");
            ConfigPath = Path.Combine(OcDir, "config.plist");
            OpenCore = File.Exists(Path.Combine(OcDir, "OpenCore.efi")) && File.Exists(ConfigPath);
            if (File.Exists(ConfigPath))
            {
                try { Plist.Load(ConfigPath); Readable = true; }
                catch (Exception e) { ReadError = e.Message; }
            }
            else ReadError = "no " + ConfigPath;

            if (string.IsNullOrEmpty(packagePath) || packagePath.Trim().Length == 0)
                PackageProblem = "no package folder given (the unpacked intel-gpu-macos-driver-VERSION-efi.zip)";
            else
            {
                PackageDir = Path.GetFullPath(packagePath.Trim().Trim('"'));
                string oc = Path.Combine(Path.Combine(PackageDir, "EFI"), "OC");
                PackageKext = Path.Combine(oc, Util.Native(KextRel));
                PackageSsdt = Path.Combine(oc, Util.Native(SsdtRel));
                PackageHpm = Path.Combine(oc, Util.Native(HpmRel));
                if (!File.Exists(Path.Combine(PackageKext, Util.Native(R.Str("kext.executable")))) || !File.Exists(PackageSsdt))
                    PackageProblem = "no EFI/OC/" + KextRel + " and EFI/OC/" + SsdtRel + " in " + PackageDir;
            }

            BackupRoot = string.IsNullOrEmpty(backupRoot) || backupRoot.Trim().Length == 0 ? DefaultBackupRoot(RootDir) : Path.GetFullPath(backupRoot.Trim().Trim('"'));
            BackupKey = Key();
            if (Util.Inside(BackupRoot, EfiDir)) throw new EfiException("the backup folder " + BackupRoot + " is inside the EFI folder: choose another one");
        }

        /// Next to the EFI folder; for a small volume (an EFI partition: its space is for booting) in LOCALAPPDATA.
        public static string DefaultBackupRoot(string rootDir)
        {
            long total = -1;
            try
            {
                string root = Path.GetPathRoot(rootDir);
                if (!string.IsNullOrEmpty(root) && !root.StartsWith(@"\\")) total = new DriveInfo(root).TotalSize;
            }
            catch (Exception) { }
            if (total >= 2000L * 1000 * 1000) return Path.Combine(rootDir, "IntelXe-EFI-backups");
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrEmpty(local)) local = Path.GetTempPath();
            return Path.Combine(Path.Combine(local, "intel-gpu-macos-driver"), "efi-backups");
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool GetVolumeInformationW(string root, StringBuilder name, int nameSize, out uint serial,
            out uint maxComponent, out uint flags, StringBuilder fileSystem, int fileSystemSize);

        /// The backups' subfolder for this EFI: the volume's serial number and the folder's path on it, so that the
        /// same EFI partition is found again under another drive letter.
        string Key()
        {
            string root = Path.GetPathRoot(EfiDir) ?? "";
            string rel = EfiDir.Substring(Math.Min(root.Length, EfiDir.Length));
            string tail = Regex.Replace(rel, "[^A-Za-z0-9]+", "_").Trim('_');
            if (tail.Length > 40) tail = tail.Substring(tail.Length - 40);
            if (Environment.OSVersion.Platform == PlatformID.Win32NT && root.Length > 0)
            {
                try
                {
                    uint serial, max, flags;
                    if (GetVolumeInformationW(root, new StringBuilder(261), 261, out serial, out max, out flags, new StringBuilder(261), 261))
                        return serial.ToString("X8") + (tail.Length > 0 ? "-" + tail : "");
                }
                catch (Exception) { }
            }
            return "folder-" + Util.Sha256Text(EfiDir.ToUpperInvariant()).Substring(0, 12);
        }

        public string Describe()
        {
            string vol = "";
            try
            {
                string root = Path.GetPathRoot(EfiDir);
                if (!string.IsNullOrEmpty(root) && !root.StartsWith(@"\\"))
                {
                    var d = new DriveInfo(root);
                    if (d.IsReady)
                        vol = " (" + (d.VolumeLabel.Length > 0 ? d.VolumeLabel + ", " : "") + d.DriveFormat + ", " + Util.Size(d.TotalSize) + ")";
                }
            }
            catch (Exception) { }
            return "the folder " + EfiDir + vol;
        }

        // ---- the card's ACPI path ---------------------------------------------------------------------------------
        static string Unpad(string s) { string t = s.TrimEnd('_'); return t.Length == 0 ? s : t; }

        /// Device Manager's "Location paths" line, ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000),
        /// as the ACPI path of the bridge levels (cards.port_levels_above_gpu: two) PCI levels above the GPU
        /// (\_SB.PC00.PEG1.PEGP); "none" when that bridge has no ACPI name; null when the line is not such a path.
        public static string ConvertLocationPath(string line, long levels)
        {
            string[] parts = line.Trim().Split('#');
            var names = new List<string>();
            bool pci = false;
            foreach (string raw in parts)
            {
                string q = raw.Trim();
                Match m = Regex.Match(q, @"^ACPI\(([A-Za-z0-9_]{1,4})\)$", RegexOptions.IgnoreCase);
                if (m.Success) { if (pci) return null; names.Add(m.Groups[1].Value.ToUpperInvariant()); continue; }
                if (Regex.IsMatch(q, @"^(PCI|PCIROOT)\([0-9A-Fa-f]+\)$", RegexOptions.IgnoreCase)) { pci = true; continue; }
                return null;
            }
            if (names.Count == 0 || parts.Length < levels + 1) return null;
            int up = parts.Length - 1 - (int)levels;   // the GPU is the last part, its switch's downstream port the one before
            if (up >= names.Count) return "none";
            var path = new List<string>();
            for (int i = 0; i <= up; i++) path.Add(Unpad(names[i]));
            if (path.Count < 2) return null;
            return "\\" + string.Join(".", path.ToArray());
        }

        /// \_SB.PC00.PEG1.PEGP, \_SB_.PC00.PEG1.PEGP (Linux) or a Location paths line, as \_SB.PC00.PEG1.PEGP.
        public static string NormalizeCard(string input, long levels)
        {
            if (input == null || input.Trim().Length == 0) return "";
            string s = input.Trim().Trim('"', '\'');
            if (s.Equals("none", StringComparison.OrdinalIgnoreCase)) return "none";
            if (s.IndexOf('#') >= 0 || s.StartsWith("ACPI(", StringComparison.OrdinalIgnoreCase) || s.StartsWith("PCIROOT(", StringComparison.OrdinalIgnoreCase))
            {
                string r = ConvertLocationPath(s, levels);
                if (r == null) throw new EfiException("'" + s + "' is not a Location path with ACPI(...) parts, such as ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)");
                return r;
            }
            if (!Regex.IsMatch(s, @"^\\?[A-Za-z0-9_]{1,4}(\.[A-Za-z0-9_]{1,4})+$"))
                throw new EfiException("'" + s + "' is not an ACPI path such as \\_SB.PC00.PEG1.PEGP");
            string[] segs = s.TrimStart('\\').ToUpperInvariant().Split('.');
            for (int i = 0; i < segs.Length; i++) segs[i] = Unpad(segs[i]);
            return "\\" + string.Join(".", segs);
        }

        public void SetCard(string input) { Card = NormalizeCard(input, R.Int("cards.port_levels_above_gpu")); }

        /// Do the table's bytes name the card's port? \_SB.PC00.PEG1.PEGP is 2f 04 "_SB_PC00PEG1PEGP" in AML (Aml.NamesPath).
        public bool AmlNamesCard(string file)
        {
            if (Card == "" || Card == "none" || !File.Exists(file)) return false;
            return Aml.NamesPath(File.ReadAllBytes(file), Card, R.Str("ssdt.port_pattern"));
        }

        /// The table this card needs (ssdtTable): the package's, or the package's adapted to the card's port. adapt: yes
        /// (the package's names the port), adapted, or no (adaptWhy says why, for a port that is given).
        string adapt = "no", adaptWhy = "";
        byte[] ssdtTable;
        void PrepareSsdt()
        {
            adapt = "no"; adaptWhy = ""; ssdtTable = null;
            if (PackageProblem != null) return;
            byte[] pkg = File.ReadAllBytes(PackageSsdt);
            ssdtTable = pkg;
            if (Aml.NamesPath(pkg, Card, R.Str("ssdt.port_pattern"))) adapt = "yes";
            else if (Card != "" && Card != "none")
            {
                string why;
                byte[] t = Aml.Adapt(pkg, Card, ShippedPath, R.Str("ssdt.port_pattern"), SsdtName, out why);
                if (t != null) { adapt = "adapted"; ssdtTable = t; }
                else adaptWhy = why;
            }
        }

        // ---- HybridPM.kext: only for Alder Lake / Raptor Lake CPUs with efficiency cores --------------------------------
        /// By the CPU's name, as efi.sh: a name that matches one of hybridpm.cpu_include (i3/i5/i7/i9-12xxx..14xxx) and
        /// none of hybridpm.cpu_exclude (Core Ultra, which HybridPM leaves alone; the desktop models with P-cores only).
        /// The expressions are POSIX extended ones that mean the same in .NET (docs/design/DESIGN-efi-rules.md).
        public static bool HybridCpu(Rules r, string name)
        {
            if (string.IsNullOrEmpty(name)) return false;
            bool yes = false;
            foreach (string re in r.Strings("hybridpm.cpu_include")) if (Regex.IsMatch(name, re)) yes = true;
            foreach (string re in r.Strings("hybridpm.cpu_exclude")) if (Regex.IsMatch(name, re)) yes = false;
            return yes;
        }
        public bool HpmNeeded { get { return HybridCpu(R, Cpu); } }
        /// Does the package have HybridPM.kext (0.1.4 and later)?
        public bool HpmPackage
        {
            get
            {
                return PackageProblem == null && File.Exists(Path.Combine(PackageHpm, Util.Native(R.Str("hybridpm.executable"))))
                    && File.Exists(Path.Combine(PackageHpm, Util.Native(R.Str("hybridpm.info_plist"))));
            }
        }

        // ---- states -----------------------------------------------------------------------------------------------
        /// The executable and Info.plist of two kexts (the rule's "executable" and "info_plist"), byte for byte.
        bool SameKext(string a, string b, string rule)
        {
            foreach (string p in new[] { R.Str(rule + ".executable"), R.Str(rule + ".info_plist") })
                if (!Util.SameFile(Path.Combine(a, Util.Native(p)), Path.Combine(b, Util.Native(p)))) return false;
            return true;
        }
        public string KextState()
        {
            if (!Readable) return "unknown";
            if (!Util.Exists(EfiKext)) return "absent";
            if (PackageProblem != null) return "present";
            return SameKext(EfiKext, PackageKext, "kext") ? "same" : "different";
        }
        /// Against the table this card needs: the package's, or the package's adapted to the card's port.
        public string SsdtState()
        {
            if (!Readable) return "unknown";
            if (!Util.Exists(EfiSsdt)) return "absent";
            if (PackageProblem != null) return "present";
            PrepareSsdt();
            return File.Exists(EfiSsdt) && Util.SameBytes(File.ReadAllBytes(EfiSsdt), ssdtTable) ? "same" : "different";
        }
        /// As KextState; "different" also for an earlier build (org.hackintosh.HybridPM) and when the package has none.
        public string HpmState()
        {
            if (!Readable) return "unknown";
            if (!Util.Exists(EfiHpm)) return "absent";
            if (PackageProblem != null) return "present";
            return HpmPackage && SameKext(EfiHpm, PackageHpm, "hybridpm") ? "same" : "different";
        }
        public static string KextVersion(string kext)
        {
            try { return Plist.Raw(Plist.Load(Path.Combine(Path.Combine(kext, "Contents"), "Info.plist")).Get("CFBundleVersion")); }
            catch (Exception) { return ""; }
        }
        static string BundleId(string kext)
        {
            try { return Plist.Raw(Plist.Load(Path.Combine(Path.Combine(kext, "Contents"), "Info.plist")).Get("CFBundleIdentifier")); }
            catch (Exception) { return ""; }
        }

        static string[] Cat(string[] a, params string[] b)
        {
            var r = new string[a.Length + b.Length];
            a.CopyTo(r, 0);
            b.CopyTo(r, a.Length);
            return r;
        }
        static int FindEntry(Plist cfg, string[] array, string field, string value)
        {
            XmlElement arr = cfg.Get(array);
            if (arr == null || arr.Name != "array") return -1;
            List<XmlElement> items = Plist.Elements(arr);
            string want = value.ToUpperInvariant();
            for (int i = 0; i < items.Count; i++)
                if (Plist.Raw(Plist.DictValue(items[i], field)).ToUpperInvariant() == want) return i;
            return -1;
        }
        string EntryState(Plist cfg, string[] array, string field, string value)
        {
            if (cfg == null) return "unknown";
            int i = FindEntry(cfg, array, field, value);
            if (i < 0) return "absent";
            return Plist.Raw(cfg.Get(Cat(array, i.ToString(), "Enabled"))) == "true" ? "enabled" : "disabled";
        }

        /// The package's kexts (IntelXe.kext, and HybridPM.kext when it has one) and table against its SHA256SUMS:
        /// "ok", "no SHA256SUMS", or what differs. Every file of the kexts must be listed.
        public string CheckSums()
        {
            if (PackageProblem != null) return "no package";
            string sums = Path.Combine(PackageDir, "SHA256SUMS");
            if (!File.Exists(sums)) return "no SHA256SUMS";
            var listed = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (string raw in File.ReadAllLines(sums))
            {
                Match m = Regex.Match(raw.TrimEnd('\r'), @"^([0-9a-fA-F]{64}) [ *](.+)$");
                if (!m.Success) continue;
                string p = m.Groups[2].Value;
                if (p.StartsWith("./")) p = p.Substring(2);
                if (p.StartsWith("EFI/OC/" + KextRel + "/", StringComparison.Ordinal) || p.StartsWith("EFI/OC/" + HpmRel + "/", StringComparison.Ordinal)
                    || p == "EFI/OC/" + SsdtRel) listed[p] = m.Groups[1].Value.ToLowerInvariant();
            }
            var files = new List<string>();
            files.Add("EFI/OC/" + SsdtRel);
            foreach (string kext in new[] { KextRel, HpmRel })
            {
                string kroot = Path.GetFullPath(kext == KextRel ? PackageKext : PackageHpm);
                if (!Directory.Exists(kroot)) continue;   // HybridPM.kext: 0.1.4 and later
                foreach (string f in Directory.GetFiles(kroot, "*", SearchOption.AllDirectories))
                    files.Add("EFI/OC/" + kext + "/" + f.Substring(kroot.Length).TrimStart('\\', '/').Replace('\\', '/'));
            }
            foreach (string f in files)
            {
                string h;
                if (!listed.TryGetValue(f, out h)) return "not listed in SHA256SUMS: " + f;
                if (Util.Sha256File(Path.Combine(PackageDir, Util.Native(f))) != h) return "differs from SHA256SUMS: " + f;
            }
            foreach (string f in listed.Keys)
                if (!File.Exists(Path.Combine(PackageDir, Util.Native(f)))) return "missing, but in SHA256SUMS: " + f;
            return "ok";
        }

        // ---- plans ------------------------------------------------------------------------------------------------
        void NeedEfi()
        {
            if (!OpenCore) throw new EfiException("no OC/OpenCore.efi and OC/config.plist in " + EfiDir + ": not an OpenCore EFI");
            if (!Readable) throw new EfiException("cannot read " + ConfigPath + ": " + ReadError);
        }

        public Plan MakePlan(string kind)
        {
            NeedEfi();
            var p = new Plan();
            p.Kind = kind;
            p.Where = Describe();
            Plist cfg = Plist.Load(ConfigPath);
            switch (kind)
            {
                case "settings": PlanItems(p, cfg, "settings"); break;
                case "driver":
                    if (PackageProblem != null) throw new EfiException(PackageProblem);
                    PlanFiles(p, cfg);
                    break;
                case "undo": PlanUndo(p); break;
                default: throw new EfiException("no such plan: " + kind + " (settings, driver or undo)");
            }
            return p;
        }

        /// What a new entry sets, for the plan: "ExecutablePath ..., PlistPath ..., Enabled".
        string EntrySummary(string rule)
        {
            return "ExecutablePath " + R.Str(rule + ".entry.ExecutablePath") + ", PlistPath " + R.Str(rule + ".entry.PlistPath") + ", Enabled";
        }
        /// Item i of the array at path brought to the rule's entry in each of the rule's "fields".
        void PlanFields(Plan p, Plist cfg, string rule, string[] array, int i, string name)
        {
            string[] e = Cat(array, i.ToString());
            foreach (string f in R.Strings(rule + ".fields"))
            {
                PValue want = Rules.Value(R.Get(rule + ".entry." + f));
                string cur = Plist.Raw(cfg.Get(Cat(e, f)));
                if (cur == want.Raw) continue;
                if (want.Type == "bool")
                    p.Lines.Add("config   " + Plist.Show(array) + ", item " + i + " (" + name + "): " + f + " " + (want.Raw == "true" ? "false" : "true") + " -> " + want.Raw);
                else
                    p.Lines.Add("config   " + Plist.Show(array) + ", item " + i + " (" + name + "): " + f + " '" + cur + "' -> '" + want.Raw + "'");
                p.Ops.Add(Op.Set(Cat(e, f), want));
            }
        }

        void PlanFiles(Plan p, Plist cfg)
        {
            string ks = KextState(), cur;
            if (CheckSums() == "no SHA256SUMS") p.Notes.Add("the package has no SHA256SUMS: its files are not checked");
            if (ks == "absent")
            {
                p.Lines.Add("copy     EFI/OC/" + KextRel + " (version " + KextVersion(PackageKext) + ")");
                p.Ops.Add(Op.Copy(PackageKext, KextRel));
            }
            else if (ks == "different")
            {
                p.Lines.Add("replace  EFI/OC/" + KextRel + " (version " + KextVersion(EfiKext) + " in the EFI, " + KextVersion(PackageKext) + " in the package; the files differ)");
                p.Ops.Add(Op.Copy(PackageKext, KextRel));
            }
            int i = FindEntry(cfg, KernelAdd, "BundlePath", KextName);
            if (i < 0)
            {
                p.Lines.Add("config   Kernel > Add: add " + KextName + " (" + EntrySummary("kext") + ")");
                p.Ops.Add(Op.Append(KernelAdd, Rules.Value(R.Dict("kext.entry"))));
            }
            else PlanFields(p, cfg, "kext", KernelAdd, i, KextName);
            foreach (string old in R.Strings("kext.earlier_bundles"))
            {
                i = FindEntry(cfg, KernelAdd, "BundlePath", old);
                if (i >= 0 && Plist.Raw(cfg.Get(Cat(KernelAdd, i.ToString(), "Enabled"))) == "true")
                {
                    p.Lines.Add("config   Kernel > Add, item " + i + " (" + old + ", the earlier name of " + KextName + "): Enabled true -> false (only one of the two may load)");
                    p.Ops.Add(Op.Set(Cat(KernelAdd, i.ToString(), "Enabled"), PValue.Bool(false)));
                }
            }

            // SSDT-ARCOFF only for a port the user confirmed, and only when a table names it: the shipped one names
            // ssdt.shipped_port; for another port, the shipped one adapted to it (Aml.Adapt)
            PrepareSsdt();
            string ss = SsdtState(), dir = R.Str("ssdt.folder");
            bool efiok = ss != "absent" && AmlNamesCard(EfiSsdt);
            if (Card == "")
            {
                string seen = "";
                if (CardDetected == "none") seen = " Windows reports a Location path for the B580 here in which the card's port has no ACPI name.";
                else if (CardDetected != "") seen = " Windows reports the card's port here as " + CardDetected + ".";
                p.Notes.Add("not added: " + SsdtName + ". Add it only after macOS is installed and install.sh --apply has run (stage 3), and only for your card's ACPI path: the package's table is for " + ShippedPath +
                    " (for another path it is adapted to that one). Check it in Device Manager (the B580 > Properties > Details > Location paths), then confirm it (-CardAcpiPath, or the SSDT-ARCOFF box in the window); stage 4, step 2 of README-INSTALL.md." + seen);
            }
            else if (Card == "none")
                p.Notes.Add("not added: " + SsdtName + ". The B580's upstream port has no ACPI name on this board; the table needs one more bridge level (stage 4, step 2 of README-INSTALL.md; please open an issue)");
            else if (!efiok && adapt == "no")
                p.Notes.Add("not added: " + SsdtName + ". The package's table is for " + ShippedPath + "; the card's port here is " + Card + ", and " + adaptWhy + ". Put that path into SSDT-ARCOFF.dsl, compile it with iasl and copy it to EFI/OC/" + dir + " (stage 4, step 2 of README-INSTALL.md); then this adds its entry");
            else
            {
                if (efiok && ss == "different")
                    p.Notes.Add("kept: EFI/OC/" + SsdtRel + " differs from " + (adapt == "adapted" ? "the package's adapted to this card's port" : "the package's") +
                        " but names this card's port (" + Card + "): yours stays");
                else if (ss != "same")
                {
                    string verb = (ss == "absent" ? "copy" : "replace").PadRight(8);
                    if (adapt == "adapted")
                    {
                        p.Lines.Add(verb + " EFI/OC/" + SsdtRel + " (the package's table adapted to this card's port: names " + Card + " instead of " + ShippedPath + ")");
                        p.Ops.Add(Op.CopyBytes(ssdtTable, SsdtRel));
                    }
                    else
                    {
                        p.Lines.Add(verb + " EFI/OC/" + SsdtRel + " (names " + Card + ", this card's port)");
                        p.Ops.Add(Op.Copy(PackageSsdt, SsdtRel));
                    }
                }
                i = FindEntry(cfg, AcpiAdd, "Path", SsdtName);
                if (i < 0)
                {
                    p.Lines.Add("config   ACPI > Add: add " + SsdtName + " (Enabled)");
                    p.Ops.Add(Op.Append(AcpiAdd, Rules.Value(R.Dict("ssdt.entry"))));
                }
                else PlanFields(p, cfg, "ssdt", AcpiAdd, i, SsdtName);
            }

            PlanHybridPM(p, cfg);   // after every change above that names an item by its index: it may insert one

            // so that OpenCore records which EFI it booted from (boot_path of the rules; efi.sh and IntelXe Manager on
            // macOS find it by that). Windows cannot read whether it does already: BootPathRecorded stays false.
            string[] bp = R.KeyPath("boot_path.path");
            long bits = R.Number("boot_path.bits"), v;
            cur = Plist.Raw(cfg.Get(bp));
            if (!BootPathRecorded && Regex.IsMatch(cur, "^[0-9]+$") && long.TryParse(cur, out v) && (v & bits) != bits)
            {
                p.Lines.Add("config   " + Plist.Show(bp) + ": " + cur + " -> " + (v | bits) + " (OpenCore then records which EFI it booted from, for efi.sh on macOS)");
                p.Ops.Add(Op.Set(bp, PValue.Int(v | bits)));
            }
        }

        /// HybridPM.kext with an entry right below Lilu.kext's (hybridpm.after: a Lilu plugin loads after Lilu), only
        /// where it is needed and Lilu is loaded; an earlier build under the same name (hybridpm.earlier_builds) is replaced.
        void PlanHybridPM(Plan p, Plist cfg)
        {
            if (!HpmNeeded) return;
            if (!HpmPackage)
            {
                p.Notes.Add("not added: " + HpmName + ". This CPU (" + Cpu + ") has efficiency cores and macOS holds it below its clocks, but this package has no " + HpmName + " (it comes with 0.1.4 and later)");
                return;
            }
            int l = FindEntry(cfg, KernelAdd, "BundlePath", LiluName);
            if (l < 0 || Plist.Raw(cfg.Get(Cat(KernelAdd, l.ToString(), "Enabled"))) != "true" || !Util.Exists(EfiLilu))
            {
                p.Notes.Add("not added: " + HpmName + ". This CPU (" + Cpu + ") has efficiency cores and " + HpmName + " corrects its clocks, but it is a Lilu plugin and this EFI does not load " + LiluName +
                    " (no file in EFI/OC/" + R.Str("hybridpm.folder") + ", or its Kernel > Add entry is missing or off). Add Lilu (github.com/acidanthera/Lilu), then run this again (README-INSTALL.md, Hybrid Intel CPUs)");
                return;
            }
            string hs = HpmState();
            if (hs == "absent")
            {
                p.Lines.Add("copy     EFI/OC/" + HpmRel + " (version " + KextVersion(PackageHpm) + "; this CPU has efficiency cores: it corrects their clocks)");
                p.Ops.Add(Op.Copy(PackageHpm, HpmRel));
            }
            else if (hs == "different")
            {
                string id = BundleId(EfiHpm);
                if (R.Strings("hybridpm.earlier_builds").Contains(id))
                    p.Lines.Add("replace  EFI/OC/" + HpmRel + " (an earlier build, " + id + " " + KextVersion(EfiHpm) + ", by the package's " + KextVersion(PackageHpm) + ")");
                else
                    p.Lines.Add("replace  EFI/OC/" + HpmRel + " (version " + KextVersion(EfiHpm) + " in the EFI, " + KextVersion(PackageHpm) + " in the package; the files differ)");
                p.Ops.Add(Op.Copy(PackageHpm, HpmRel));
            }
            int h = FindEntry(cfg, KernelAdd, "BundlePath", HpmName);
            if (h < 0)
            {
                p.Lines.Add("config   Kernel > Add: add " + HpmName + " below " + LiluName + ", as item " + (l + 1) + " (" + EntrySummary("hybridpm") + ")");
                p.Ops.Add(Op.Insert(KernelAdd, l + 1, Rules.Value(R.Dict("hybridpm.entry"))));
                return;
            }
            PlanFields(p, cfg, "hybridpm", KernelAdd, h, HpmName);
            if (h < l)
                p.Notes.Add(HpmName + " is item " + h + " of Kernel > Add, above " + LiluName + " (item " + l + "): OpenCore loads it before Lilu and it does not start. Move its entry below " + LiluName + "'s");
        }

        /// csr-active-config as a number: its first four bytes, little-endian.
        public static long CsrValue(byte[] b)
        {
            long v = 0;
            if (b == null) return 0;
            for (int i = 0; i < 4 && i < b.Length; i++) v |= (long)b[i] << (8 * i);
            return v;
        }
        public static byte[] CsrBytes(long v)
        {
            return new byte[] { (byte)(v & 255), (byte)(v >> 8 & 255), (byte)(v >> 16 & 255), (byte)(v >> 24 & 255) };
        }

        /// The items of the rules' "settings" (stage 2: SIP, AMFI and surfvram boot-args, quirks) or "safari" list
        /// (SecureBootModel for the system-volume copy), in their order; the kinds are described in
        /// docs/design/DESIGN-efi-rules.md. The same lines and steps as efi.sh's plan_items.
        void PlanItems(Plan p, Plist cfg, string list)
        {
            int n = R.List(list).Count;
            for (int i = 0; i < n; i++)
            {
                string it = list + "." + i + ".", kind = R.Str(it + "kind"), why = R.StrOr(it + "why");
                string[] path = R.KeyPath(it + "path");
                string show = Plist.Show(path), tail = why == "" ? "" : " (" + why + ")";
                XmlElement now = cfg.Get(path);
                string cur = Plist.Raw(now);
                switch (kind)
                {
                    case "value":
                    case "value_if":
                    {
                        PValue want = R.Value(it + "value", R.Str(it + "type"));
                        if (kind == "value_if" ? cur != Rules.Value(R.Get(it + "if")).Raw : cur == want.Raw) break;
                        p.Lines.Add("config   " + show + ": " + (cur == "" ? "missing" : cur) + " -> " + want.Raw + tail);
                        p.Ops.Add(Op.Set(path, want));
                        break;
                    }
                    case "require":
                        if (now == null) { p.Notes.Add(R.Str(it + "note")); return; }
                        break;
                    case "dict":
                        if (now == null)
                        {
                            p.Lines.Add("config   " + show + ": add (missing)");
                            p.Ops.Add(Op.Make("mkdict", path));
                        }
                        break;
                    case "bits":
                    {
                        string type = R.Str(it + "type");
                        long bits = R.Number(it + "bits"), v;
                        if (type == "data") v = CsrValue(Plist.DataBytes(now));
                        else if (type == "integer") { if (!Regex.IsMatch(cur, "^[0-9]+$") || !long.TryParse(cur, out v)) v = 0; }
                        else throw new EfiException(R.Source + ": " + list + " item " + i + ": bits of a " + type);
                        if ((v & bits) == bits) break;
                        p.Lines.Add("config   " + show + ": 0x" + v.ToString("x") + " -> 0x" + (v | bits).ToString("x") + tail);
                        p.Ops.Add(Op.Set(path, type == "data" ? PValue.Data(CsrBytes(v | bits)) : PValue.Int(v | bits)));
                        break;
                    }
                    case "boot_args":
                    {
                        var more = new List<string>();
                        int m = R.List(it + "args").Count;
                        for (int j = 0; j < m; j++)
                            if (!Regex.IsMatch(" " + cur + " ", R.Str(it + "args." + j + ".unless"))) more.Add(R.Str(it + "args." + j + ".add"));
                        if (more.Count == 0) break;
                        string joined = string.Join(" ", more.ToArray()), after = (cur + " " + joined).TrimStart(' ');
                        p.Lines.Add("config   " + show + ": add " + joined + " ('" + cur + "' -> '" + after + "')");
                        p.Ops.Add(Op.Set(path, PValue.Str(after)));
                        break;
                    }
                    case "strings":
                    {
                        var listed = new List<string>();
                        if (now == null)
                        {
                            p.Lines.Add("config   " + show + ": add (missing)");
                            p.Ops.Add(Op.Make("mkarray", path));
                        }
                        else foreach (XmlElement e in Plist.Elements(now)) if (e.Name == "string") listed.Add(e.InnerText);
                        foreach (string name in R.Strings(it + "strings"))
                            if (!listed.Contains(name))
                            {
                                p.Lines.Add("config   " + show + ": add " + name + tail);
                                p.Ops.Add(Op.Append(path, PValue.Str(name)));
                            }
                        break;
                    }
                    default:
                        throw new EfiException(R.Source + ": " + list + " item " + i + " is of the kind '" + kind + "', which this tool does not know (a newer rules file?)");
                }
            }
        }

        // ---- backups ----------------------------------------------------------------------------------------------
        List<string> Backups()
        {
            var list = new List<string>();
            if (!Directory.Exists(BackupDir)) return list;
            foreach (string d in Directory.GetDirectories(BackupDir))
                if (Regex.IsMatch(Path.GetFileName(d), "^[0-9]{3}-")) list.Add(d);
            list.Sort(StringComparer.Ordinal);
            return list;
        }
        /// The newest backup not yet put back, or null.
        public string LastBackup()
        {
            List<string> all = Backups();
            for (int i = all.Count - 1; i >= 0; i--) if (!File.Exists(Path.Combine(all[i], "restored"))) return all[i];
            return null;
        }
        static List<string> Changed(string backup)
        {
            var list = new List<string>();
            string f = Path.Combine(backup, "changed");
            if (File.Exists(f)) foreach (string l in File.ReadAllLines(f)) if (l.Trim().Length > 0) list.Add(l.Trim());
            return list;
        }

        void PlanUndo(Plan p)
        {
            string b = LastBackup();
            if (b == null) { p.Notes.Add("no backup to put back for this EFI"); return; }
            p.Lines.Add("from     " + b + " (" + Util.ReadLine1(Path.Combine(b, "kind"), "change") + ", " + Util.ReadLine1(Path.Combine(b, "date"), "") + ")");
            List<string> may = R.Strings("undo_paths");
            foreach (string rel in Changed(b))
            {
                if (!may.Contains(rel)) { p.Notes.Add("the backup lists EFI/OC/" + rel + ", which these rules never change: left as it is"); continue; }
                if (Util.Exists(Path.Combine(Path.Combine(Path.Combine(b, "EFI"), "OC"), Util.Native(rel)))) p.Lines.Add("restore  EFI/OC/" + rel);
                else p.Lines.Add("remove   EFI/OC/" + rel + " (it was not there before)");
                p.Ops.Add(Op.Restore(rel));
            }
            string after = Path.Combine(b, "config-after.sha256");
            if (File.Exists(after) && File.Exists(ConfigPath) && Util.Sha256File(ConfigPath) != Util.ReadLine1(after, ""))
                p.Notes.Add("config.plist was changed after that change: putting it back loses those later edits");
        }

        // ---- doing it ---------------------------------------------------------------------------------------------
        public void CheckWritable()
        {
            string probe = Path.Combine(OcDir, ".intelxe-write-test");
            try { File.WriteAllText(probe, "x"); File.Delete(probe); }
            catch (UnauthorizedAccessException)
            {
                throw new EfiException("cannot write to " + OcDir + ". An EFI partition mounted with mountvol can be written only by an administrator: start PowerShell as administrator (or use the window's \"Restart as administrator\")");
            }
            catch (IOException e) { throw new EfiException("cannot write to " + OcDir + ": " + e.Message); }
        }

        string Backup(string kind, Action<string> say)
        {
            int files;
            long size = Util.TreeSize(EfiDir, out files);
            Directory.CreateDirectory(BackupDir);
            long free = Util.FreeSpace(BackupDir);
            if (free >= 0 && free < size + 1000000)
                throw new EfiException("not enough space for a backup of the EFI folder (" + Util.Size(size) + ") in " + BackupDir + ": choose another backup folder. Nothing changed");
            int n = 0;
            foreach (string d in Backups()) { int k; if (int.TryParse(Path.GetFileName(d).Substring(0, 3), out k) && k > n) n = k; }
            string b = Path.Combine(BackupDir, (n + 1).ToString("000") + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            Directory.CreateDirectory(b);
            Util.CopyTree(EfiDir, Path.Combine(b, "EFI"));
            int copied;
            long csize = Util.TreeSize(Path.Combine(b, "EFI"), out copied);
            if (copied != files || csize != size)
                throw new EfiException("the backup in " + b + " is incomplete (" + copied + " of " + files + " files): nothing changed in the EFI");
            Util.WriteText(Path.Combine(b, "kind"), kind + "\n");
            Util.WriteText(Path.Combine(b, "date"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "\n");
            Util.WriteText(Path.Combine(b, "where"), Describe() + "\n");
            Util.WriteText(Path.Combine(b, "changed"), "");
            say("== backup of the whole EFI folder: " + b);
            return b;
        }

        /// Make the plan of kind (settings, driver, undo) if it is still the one whose hash the user saw.
        public void Run(string kind, string expect, Action<string> say)
        {
            Plan p = MakePlan(kind);
            if (kind == "driver")
            {
                string sums = CheckSums();
                if (sums != "ok" && sums != "no SHA256SUMS")
                    throw new EfiException("the package's EFI files differ from its SHA256SUMS (damaged or changed): nothing changed (" + sums + ")");
            }
            if (string.IsNullOrEmpty(expect))
                throw new EfiException("nothing changed: give the SHA-256 of the plan you read (-Expect; -Plan " + kind + " prints it)");
            if (expect.Trim().ToLowerInvariant() != p.Hash)
                throw new EfiException("the EFI changed since the plan was shown (plan " + p.Hash + ", expected " + expect.Trim() + "): nothing changed");
            foreach (string l in p.Text.TrimEnd('\n').Split('\n')) say(l);
            if (kind == "undo") { RunUndo(say); return; }
            if (!p.HasChanges) { say("== nothing to change"); return; }
            CheckWritable();

            // config.plist edited in memory first: if it does not read back, nothing is touched. Each value is checked
            // right after it is set: a later insert (HybridPM.kext's entry) moves the items below it, so an index named
            // by an earlier step may hold another entry at the end.
            byte[] edited = null;
            Plist cfg = Plist.Load(ConfigPath);
            bool any = false;
            foreach (Op op in p.Ops)
            {
                if (op.Kind == "copy") continue;
                cfg.Apply(op);
                any = true;
                if (op.Kind == "set" && Plist.Raw(cfg.Get(op.Path)) != op.Value.Raw)
                    throw new EfiException("the edited config.plist does not hold " + Plist.Show(op.Path) + " = " + op.Value.Raw + ": nothing changed");
            }
            if (any)
            {
                edited = cfg.ToBytes();
                Plist check = Plist.Parse(edited, "the edited config.plist");
                if (Plist.Canonical(check.Root) != Plist.Canonical(cfg.Root))
                    throw new EfiException("the edited config.plist does not read back as edited: nothing changed");
            }
            long need = edited == null ? 0 : edited.Length;
            foreach (Op op in p.Ops)
                if (op.Kind == "copy") { int f; need += op.Bytes != null ? op.Bytes.Length : Util.TreeSize(op.Source, out f); }
            long free = Util.FreeSpace(EfiDir);
            if (free >= 0 && free < need + 1000000)
                throw new EfiException("not enough space on the EFI's volume (" + Util.Size(free) + " free): nothing changed");

            string b = Backup(R.Str("backup_kinds." + kind), say);
            string changed = Path.Combine(b, "changed");
            foreach (Op op in p.Ops)
            {
                if (op.Kind != "copy") continue;
                File.AppendAllText(changed, op.Rel + "\n");   // before the copy: an interrupted copy is undone too
                if (op.Bytes != null) Util.PutBytes(op.Bytes, Path.Combine(OcDir, Util.Native(op.Rel)));
                else Util.PutCopy(op.Source, Path.Combine(OcDir, Util.Native(op.Rel)));
                say("  ok    EFI/OC/" + op.Rel);
            }
            if (edited != null)
            {
                File.AppendAllText(changed, "config.plist\n");
                Util.Replace(ConfigPath, edited);
                Util.WriteText(Path.Combine(b, "config-after.sha256"), Util.Sha256(edited) + "\n");
                say("  ok    EFI/OC/config.plist");
            }
            say("== done. Restart to use it. To go back: Undo (one change per run); the backup stays in " + b);
        }

        void RunUndo(Action<string> say)
        {
            string b = LastBackup();
            if (b == null) throw new EfiException("no backup to put back for this EFI (backups: " + BackupDir + ")");
            CheckWritable();
            List<string> may = R.Strings("undo_paths");
            foreach (string rel in Changed(b))
            {
                if (!may.Contains(rel)) continue;   // undo puts back only what the rules change (the plan's note says so)
                string src = Path.Combine(Path.Combine(Path.Combine(b, "EFI"), "OC"), Util.Native(rel));
                string dst = Path.Combine(OcDir, Util.Native(rel));
                if (Util.Exists(src)) { Util.PutCopy(src, dst); say("  ok    restored EFI/OC/" + rel); }
                else { Util.Delete(dst); say("  ok    removed EFI/OC/" + rel); }
            }
            Util.WriteText(Path.Combine(b, "restored"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "\n");
            say("== done. The backup stays in " + b + ".");
        }

        // ---- status -----------------------------------------------------------------------------------------------
        public List<string> Status()
        {
            var s = new List<string>();
            Plist cfg = null;
            if (Readable) { try { cfg = Plist.Load(ConfigPath); } catch (Exception) { } }
            s.Add("efi_source=folder");
            s.Add("efi_folder=" + EfiDir);
            s.Add("efi_description=" + Describe());
            s.Add("opencore=" + (OpenCore ? "yes" : "no"));
            s.Add("efi_readable=" + (Readable ? "yes" : "no"));
            if (!Readable) s.Add("efi_read_error=" + ReadError);
            s.Add("package=" + PackageDir);
            s.Add("package_problem=" + (PackageProblem ?? ""));
            s.Add("package_checksums=" + CheckSums());
            s.Add("kext_file=" + KextState());
            s.Add("kext_entry=" + EntryState(cfg, KernelAdd, "BundlePath", KextName));
            s.Add("kext_version_package=" + (PackageProblem == null ? KextVersion(PackageKext) : ""));
            s.Add("kext_version_efi=" + (Readable && Util.Exists(EfiKext) ? KextVersion(EfiKext) : ""));
            List<string> olds = R.Strings("kext.earlier_bundles");
            s.Add("old_kext_entry=" + (olds.Count > 0 ? EntryState(cfg, KernelAdd, "BundlePath", olds[0]) : "absent"));
            s.Add("ssdt_file=" + SsdtState());
            s.Add("ssdt_entry=" + EntryState(cfg, AcpiAdd, "Path", SsdtName));
            s.Add("card_acpi_path=" + Card);
            s.Add("card_acpi_path_windows=" + CardDetected);
            s.Add("hybridpm_needed=" + (HpmNeeded ? "yes" : "no"));
            s.Add("hybridpm_cpu=" + Cpu);
            s.Add("hybridpm_package=" + (HpmPackage ? "yes" : "no"));
            s.Add("hybridpm_version_package=" + (HpmPackage ? KextVersion(PackageHpm) : ""));
            s.Add("hybridpm_file=" + HpmState());
            s.Add("hybridpm_entry=" + EntryState(cfg, KernelAdd, "BundlePath", HpmName));
            s.Add("hybridpm_version_efi=" + (Readable && Util.Exists(EfiHpm) ? KextVersion(EfiHpm) : ""));
            s.Add("lilu_entry=" + EntryState(cfg, KernelAdd, "BundlePath", LiluName));
            PrepareSsdt();
            s.Add("ssdt_package_names_card=" + (adapt == "yes" ? "yes" : adapt == "adapted" ? "adaptable" : "no"));
            string pc = "", sc = "";
            if (OpenCore && Readable)
            {
                if (PackageProblem == null) pc = MakePlan("driver").Lines.Count.ToString();
                sc = MakePlan("settings").Lines.Count.ToString();
            }
            s.Add("plan_changes=" + pc);
            s.Add("settings_changes=" + sc);
            s.Add("backup_root=" + BackupDir);
            string b = LastBackup();
            s.Add("backup=" + (b ?? "none"));
            s.Add("backup_kind=" + (b == null ? "" : Util.ReadLine1(Path.Combine(b, "kind"), "")));
            return s;
        }
    }
}
// END ENGINE
'@

if (-not ('IntelXeEfi.EfiFolder' -as [type])) {
    if ($PSVersionTable.PSEdition -eq 'Core') {
        Add-Type -TypeDefinition $EngineSource -Language CSharp -IgnoreWarnings
    } else {
        Add-Type -TypeDefinition $EngineSource -Language CSharp -ReferencedAssemblies 'System.Xml' -IgnoreWarnings
    }
}

# ---- Windows: rights, the card, EFI partitions ------------------------------------------------------------------------
$EspType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$ShippedPath = '\_SB.PC00.PEG1.PEGP'   # for the window's texts; the window takes the rules' ssdt.shipped_port

# The message of an error: the engine's own text, not PowerShell's "Exception calling ..." around it.
function Get-ErrorText($err) {
    $ex = $err
    if ($err -is [System.Management.Automation.ErrorRecord]) { $ex = $err.Exception }
    while ($null -ne $ex.InnerException) {
        if ($ex -is [System.Management.Automation.MethodInvocationException] -or $ex -is [System.Reflection.TargetInvocationException]) {
            $ex = $ex.InnerException
        } elseif ($ex.InnerException -is [IntelXeEfi.EfiException]) {
            $ex = $ex.InnerException
        } else { break }
    }
    return $ex.Message
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---- the EFI rules: efi-rules.json, read with ConvertFrom-Json ---------------------------------------------------------
# The rules file next to this script (the -efi.zip), else the repository's (windows\..\release\templates); -RulesFile
# names another.
function Find-RulesFile([string]$given) {
    if ($given -ne '') { return $given }
    foreach ($p in @((Join-Path $PSScriptRoot 'efi-rules.json'), (Join-Path (Split-Path -Parent $PSScriptRoot) (Join-Path 'release' (Join-Path 'templates' 'efi-rules.json'))))) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return (Join-Path $PSScriptRoot 'efi-rules.json')
}

# What ConvertFrom-Json gives, as the plain tree the engine reads: dictionaries in the file's order, lists, strings,
# integers (long) and booleans. Anything else (a fraction, null) is not a rule.
function ConvertTo-RulesTree($v, [string]$at) {
    if ($null -eq $v) { throw "efi-rules.json: $at is null" }
    if ($v -is [string]) { return $v }
    if ($v -is [bool]) { return $v }
    if ($v -is [int] -or $v -is [long]) { return [long]$v }
    if ($v -is [System.Management.Automation.PSCustomObject]) {
        $d = New-Object 'System.Collections.Generic.Dictionary[string,object]'
        # Add, not $d[...] = ...: a method call hands over the object itself, an index assignment its PSObject wrapper
        foreach ($p in $v.PSObject.Properties) { $d.Add($p.Name, (ConvertTo-RulesTree $p.Value "$at.$($p.Name)").PSObject.BaseObject) }
        return $d
    }
    if ($v -is [array]) {
        $l = New-Object 'System.Collections.Generic.List[object]'
        $i = 0
        foreach ($x in $v) { $l.Add((ConvertTo-RulesTree $x "$at.$i").PSObject.BaseObject); $i++ }
        return , $l
    }
    throw "efi-rules.json: $at is a $($v.GetType().Name), not a string, an integer, a boolean, an object or a list"
}

function Read-EfiRules([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { throw "no $($path): IntelXe-EFI.ps1 needs the efi-rules.json it came with (next to it)" }
    $json = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $path).ProviderPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
    return [IntelXeEfi.Rules]::FromTree((ConvertTo-RulesTree $json 'rules').PSObject.BaseObject, $path)
}

$script:EfiRules = $null
function Get-EfiRules {
    if ($null -eq $script:EfiRules) { $script:EfiRules = Read-EfiRules (Find-RulesFile $RulesFile) }
    return $script:EfiRules
}

# The "ACPI(...)" Location path of an Arc B580 or B570 in this PC, as Device Manager shows it; "" without one. The cards
# are the rules' (cards.vendor_id, cards.device_ids: PCI 8086:E20B / E20C, the list of data/platforms.json "supported").
function Get-CardLocation {
    $rules = Get-EfiRules
    try {
        $devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $rules.IsCardInstance($_.InstanceId) })
        foreach ($d in $devices) {
            $paths = @((Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_LocationPaths' -ErrorAction Stop).Data)
            foreach ($l in $paths) { if ("$l" -like 'ACPI(*') { return "$l" } }
        }
    } catch { }
    return ''
}

function Get-CardDetected {
    $loc = Get-CardLocation
    if ($loc -eq '') { return '' }
    $p = [IntelXeEfi.EfiFolder]::ConvertLocationPath($loc, (Get-EfiRules).Int('cards.port_levels_above_gpu'))
    if ($null -eq $p) { return '' }
    return $p
}

# The CPU's name, for HybridPM.kext: INTELXE_CPU_BRAND when it is set (the tests, as for efi.sh), else Windows'
# Win32_Processor.Name; "" when Windows does not tell.
function Get-CpuName {
    if (Test-Path -LiteralPath 'Env:INTELXE_CPU_BRAND') { return [string](Get-Item -LiteralPath 'Env:INTELXE_CPU_BRAND').Value }
    try {
        $cpu = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop) | Select-Object -First 1
        if ($null -ne $cpu -and $null -ne $cpu.Name) { return ([string]$cpu.Name).Trim() }
    } catch { }
    return ''
}

function Get-DriveLetterText($partition) {
    if ($null -ne $partition.DriveLetter -and [int][char]$partition.DriveLetter -ne 0) { return "$($partition.DriveLetter):" }
    return ''
}

# The EFI system partitions of this PC: disk, size, volume path (for mountvol), drive letter.
function Get-EspList {
    $list = @()
    foreach ($p in @(Get-Partition -ErrorAction Stop | Where-Object { "$($_.GptType)" -eq $EspType })) {
        $disk = $null
        try { $disk = Get-Disk -Number $p.DiskNumber -ErrorAction Stop } catch { }
        $volume = @($p.AccessPaths | Where-Object { "$_" -like '\\?\Volume{*' }) | Select-Object -First 1
        $name = 'unknown disk'; $bus = ''
        if ($null -ne $disk) { $name = "$($disk.FriendlyName)"; $bus = "$($disk.BusType)" }
        $list += New-Object PSObject -Property @{
            Disk = $name; Bus = $bus; DiskNumber = $p.DiskNumber; PartitionNumber = $p.PartitionNumber
            Size = [IntelXeEfi.Util]::Size([long]$p.Size); Volume = "$volume"; Letter = (Get-DriveLetterText $p); IsSystem = [bool]$p.IsSystem
        }
    }
    return $list
}

function Format-Esp($e) {
    $where = 'not mounted'
    if ($e.Letter -ne '') { $where = "mounted at $($e.Letter)" }
    $sys = ''
    if ($e.IsSystem) { $sys = ', the one Windows booted from' }
    return "$($e.Disk)$(if ($e.Bus -ne '') { " ($($e.Bus))" }), $($e.Size), disk $($e.DiskNumber) partition $($e.PartitionNumber)$sys; $where"
}

function Get-FreeLetter {
    $used = @([IO.Directory]::GetLogicalDrives() | ForEach-Object { $_.Substring(0, 1).ToUpperInvariant() })
    foreach ($l in 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'R', 'Q', 'P', 'O', 'N', 'M') {
        if ($used -notcontains $l) { return "$($l):" }
    }
    throw 'no free drive letter between M: and Z:'
}

# mountvol LETTER VOLUME (or /S: the EFI partition Windows booted from). Administrator only.
function Mount-Esp([string]$volume, [string]$letter) {
    if (-not (Test-Admin)) { throw 'mounting an EFI partition needs PowerShell started as administrator' }
    if ($letter -eq '') { $letter = Get-FreeLetter }
    $letter = $letter.TrimEnd('\').ToUpperInvariant()
    if ($letter -notmatch '^[A-Z]:$') { throw "not a drive letter: $letter" }
    $mountvol = Join-Path $env:SystemRoot 'System32\mountvol.exe'
    $ErrorActionPreference = 'Continue'   # mountvol's error text is read below, not thrown
    if ($volume -eq 'system') { $out = & $mountvol $letter /S 2>&1 } else {
        if ($volume -notmatch '^\\\\\?\\Volume\{[0-9A-Fa-f-]+\}\\$') { throw "not a volume path such as \\?\Volume{...}\ (see -ListEsp): $volume" }
        $out = & $mountvol $letter $volume 2>&1
    }
    if ($LASTEXITCODE -ne 0) { throw "mountvol failed: $out" }
    return $letter
}

function Dismount-Esp([string]$letter) {
    if (-not (Test-Admin)) { throw 'unmounting an EFI partition needs PowerShell started as administrator' }
    $letter = $letter.TrimEnd('\').ToUpperInvariant()
    if ($letter -notmatch '^[A-Z]:$') { throw "not a drive letter: $letter" }
    $p = $null
    try { $p = Get-Partition -DriveLetter $letter.Substring(0, 1) -ErrorAction Stop } catch { }
    if ($null -eq $p -or "$($p.GptType)" -ne $EspType) { throw "$letter is not an EFI partition (or Windows cannot tell): unmount it yourself with mountvol $letter /D" }
    $ErrorActionPreference = 'Continue'
    $out = & (Join-Path $env:SystemRoot 'System32\mountvol.exe') $letter /D 2>&1
    if ($LASTEXITCODE -ne 0) { throw "mountvol failed: $out" }
}

# "PATH" for a command line; E:\ becomes "E:\." so that its backslash does not escape the quote
function Format-Argument([string]$text) {
    $t = $text.Trim()
    if ($t.EndsWith('\')) { $t += '.' }
    return '"' + $t + '"'
}

# The package folder: the one given, or this script's folder (or its parent) when it holds EFI\OC\Kexts\IntelXe.kext.
function Resolve-PackageDir([string]$given) {
    if ($given -ne '') { return $given }
    foreach ($d in @($PSScriptRoot, (Split-Path -Parent $PSScriptRoot))) {
        if ($d -and (Test-Path -LiteralPath (Join-Path $d 'EFI\OC\Kexts\IntelXe.kext\Contents\MacOS\IntelXe'))) { return $d }
    }
    return ''
}

function New-EfiFolder([string]$efiPath, [string]$packagePath, [string]$backupPath, [string]$card, [string]$detected, [string]$cpu) {
    $f = [IntelXeEfi.EfiFolder]::new($efiPath, (Resolve-PackageDir $packagePath), $backupPath, (Get-EfiRules))
    if ($card -ne '') { $f.SetCard($card) }
    $f.CardDetected = $detected
    $f.Cpu = $cpu
    return $f
}

# ---- the window --------------------------------------------------------------------------------------------------------
function Show-Window {
    try { $ShippedPath = (Get-EfiRules).Str('ssdt.shipped_port') } catch { }   # without the rules each action says so
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [System.Windows.Forms.Application]::EnableVisualStyles()
    $script:mounted = ''
    $script:detected = $null
    $script:cpuName = $null

    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'IntelXe EFI (intel-gpu-macos-driver): OpenCore changes for the Arc B580'
    $form.Size = New-Object System.Drawing.Size(980, 760)
    $form.MinimumSize = New-Object System.Drawing.Size(860, 600)
    $form.StartPosition = 'CenterScreen'
    $top = [System.Windows.Forms.AnchorStyles]::Top
    $left = [System.Windows.Forms.AnchorStyles]::Left
    $right = [System.Windows.Forms.AnchorStyles]::Right
    $bottom = [System.Windows.Forms.AnchorStyles]::Bottom

    function Add-Label($text, $x, $y, $w, $h) {
        $l = New-Object System.Windows.Forms.Label
        $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, $h)
        $form.Controls.Add($l); return $l
    }
    function Add-Box($x, $y, $w) {
        $b = New-Object System.Windows.Forms.TextBox
        $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, 24)
        $b.Anchor = $top -bor $left -bor $right
        $form.Controls.Add($b); return $b
    }
    function Add-Button($text, $x, $y, $w, $anchor) {
        $b = New-Object System.Windows.Forms.Button
        $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, 28)
        $b.Anchor = $anchor
        $form.Controls.Add($b); return $b
    }

    $admin = Test-Admin
    $null = Add-Label 'EFI folder' 12 16 110 20
    $efiBox = Add-Box 125 13 570
    $efiBrowse = Add-Button 'Browse...' 703 11 90 ($top -bor $right)
    $mountButton = Add-Button 'Mount EFI partition...' 798 11 155 ($top -bor $right)
    $null = Add-Label 'Package folder' 12 50 110 20
    $pkgBox = Add-Box 125 47 570
    $pkgBrowse = Add-Button 'Browse...' 703 45 90 ($top -bor $right)
    $null = Add-Label 'Backups' 12 84 110 20
    $bkBox = Add-Box 125 81 570
    $bkBrowse = Add-Button 'Browse...' 703 79 90 ($top -bor $right)
    $hint = Add-Label 'Empty: a folder next to the EFI folder; for an EFI partition, %LOCALAPPDATA%\intel-gpu-macos-driver\efi-backups. Undo looks in the same place.' 125 107 830 18
    $hint.Anchor = $top -bor $left -bor $right

    $group = New-Object System.Windows.Forms.GroupBox
    $group.Text = 'SSDT-ARCOFF.aml (stage 4 only, after macOS is installed and install.sh --apply has run)'
    $group.Location = New-Object System.Drawing.Point(12, 130); $group.Size = New-Object System.Drawing.Size(941, 100)
    $group.Anchor = $top -bor $left -bor $right
    $form.Controls.Add($group)
    $ssdtCheck = New-Object System.Windows.Forms.CheckBox
    $ssdtCheck.Text = "Add it. My card's ACPI path is:"; $ssdtCheck.Location = New-Object System.Drawing.Point(10, 22); $ssdtCheck.Size = New-Object System.Drawing.Size(215, 22)
    $group.Controls.Add($ssdtCheck)
    $cardBox = New-Object System.Windows.Forms.TextBox
    $cardBox.Location = New-Object System.Drawing.Point(228, 22); $cardBox.Size = New-Object System.Drawing.Size(540, 24)
    $cardBox.Anchor = $top -bor $left -bor $right
    $group.Controls.Add($cardBox)
    $detectButton = New-Object System.Windows.Forms.Button
    $detectButton.Text = 'Read from Windows'; $detectButton.Location = New-Object System.Drawing.Point(776, 20); $detectButton.Size = New-Object System.Drawing.Size(155, 28)
    $detectButton.Anchor = $top -bor $right
    $group.Controls.Add($detectButton)
    $cardHint = New-Object System.Windows.Forms.Label
    $cardHint.Text = "The shipped table is for $ShippedPath; for another path it is adapted to that one. Check yours: Device Manager > Display adapters > the B580 (or Microsoft Basic Display Adapter) > Properties > Details > Location paths. ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000) is $ShippedPath; you can paste that line. Leave it unticked before macOS is installed: the table hides the B580 from the display driver the installer uses."
    $cardHint.Location = New-Object System.Drawing.Point(10, 50); $cardHint.Size = New-Object System.Drawing.Size(921, 46)
    $cardHint.Anchor = $top -bor $left -bor $right
    $group.Controls.Add($cardHint)

    $y = 240
    $statusButton = Add-Button 'Status' 12 $y 90 ($top -bor $left)
    $settingsPlan = Add-Button 'Settings: plan (stage 2)' 107 $y 170 ($top -bor $left)
    $settingsApply = Add-Button 'Settings: apply...' 282 $y 130 ($top -bor $left)
    $driverPlan = Add-Button 'Driver: plan (stage 4)' 417 $y 160 ($top -bor $left)
    $driverApply = Add-Button 'Driver: apply...' 582 $y 120 ($top -bor $left)
    $undoPlan = Add-Button 'Undo: plan' 707 $y 90 ($top -bor $left)
    $undoApply = Add-Button 'Undo last change...' 802 $y 151 ($top -bor $left)
    $y = 274
    $adminButton = Add-Button 'Restart as administrator' 12 $y 175 ($top -bor $left)
    $unmountButton = Add-Button 'Unmount EFI partition' 192 $y 160 ($top -bor $left)
    $unmountButton.Enabled = $false
    $rights = 'Running as administrator.'
    if (-not $admin) { $rights = 'Not administrator: enough for a USB stick or a copy; an EFI partition mounted with mountvol needs "Restart as administrator".' }
    $rightsLabel = Add-Label $rights 360 280 595 20
    $rightsLabel.Anchor = $top -bor $left -bor $right
    if ($admin) { $adminButton.Enabled = $false }

    $out = New-Object System.Windows.Forms.TextBox
    $out.Multiline = $true; $out.ReadOnly = $true; $out.ScrollBars = 'Both'; $out.WordWrap = $false
    $out.Font = New-Object System.Drawing.Font('Consolas', 9)
    $out.Location = New-Object System.Drawing.Point(12, 310); $out.Size = New-Object System.Drawing.Size(941, 395)
    $out.Anchor = $top -bor $left -bor $right -bor $bottom
    $form.Controls.Add($out)

    $efiBox.Text = $Efi
    $pkgBox.Text = Resolve-PackageDir $Package
    $bkBox.Text = $BackupDir
    if ($CardAcpiPath -ne '') { $cardBox.Text = $CardAcpiPath; $ssdtCheck.Checked = $true }

    $say = [Action[string]] {
        param($line)
        $out.AppendText($line + [Environment]::NewLine)
        [System.Windows.Forms.Application]::DoEvents()
    }
    function Show-Lines($lines) { foreach ($l in $lines) { $say.Invoke([string]$l) } }
    function Show-Error($text) {
        $say.Invoke("FAILED: $text")
        [void][System.Windows.Forms.MessageBox]::Show($form, $text, 'IntelXe EFI', 'OK', 'Warning')
    }
    function Ask($text, $title) {
        return [System.Windows.Forms.MessageBox]::Show($form, $text, $title, 'OKCancel', 'Question') -eq 'OK'
    }
    function Get-Folder($description, $start) {
        $d = New-Object System.Windows.Forms.FolderBrowserDialog
        $d.Description = $description
        $d.ShowNewFolderButton = $false
        if ($start -and (Test-Path -LiteralPath $start)) { $d.SelectedPath = $start }
        if ($d.ShowDialog($form) -eq 'OK') { return $d.SelectedPath }
        return $null
    }
    function Get-Detected {
        if ($null -eq $script:detected) { $script:detected = Get-CardDetected }
        return $script:detected
    }
    function Get-Cpu {
        if ($null -eq $script:cpuName) { $script:cpuName = Get-CpuName }
        return $script:cpuName
    }
    function Open-Folder {
        $card = ''
        if ($ssdtCheck.Checked) {
            $card = $cardBox.Text.Trim()
            if ($card -eq '') { throw "SSDT-ARCOFF.aml is ticked, but no ACPI path is given: type the card's path (for the shipped table $ShippedPath) or untick it" }
        }
        return New-EfiFolder $efiBox.Text $pkgBox.Text $bkBox.Text $card (Get-Detected) (Get-Cpu)
    }
    function Start-Busy($title) { $out.Clear(); $say.Invoke("== $title"); $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor }
    function Stop-Busy { $form.Cursor = [System.Windows.Forms.Cursors]::Default }

    # the plan in a dialog, with the button that makes it: true when the user chose it
    function Confirm-Plan($folder, $plan, $title, $what) {
        $d = New-Object System.Windows.Forms.Form
        $d.Text = $title; $d.Size = New-Object System.Drawing.Size(900, 560); $d.StartPosition = 'CenterParent'
        $d.MinimizeBox = $false; $d.MaximizeBox = $false
        $l = New-Object System.Windows.Forms.Label
        $l.Text = "$what`r`nBackups go to: $($folder.BackupDir)`r`nIf the EFI changes before you confirm, nothing is written. Restart the PC afterwards."
        $l.Location = New-Object System.Drawing.Point(12, 10); $l.Size = New-Object System.Drawing.Size(860, 60); $l.Anchor = $top -bor $left -bor $right
        $d.Controls.Add($l)
        $t = New-Object System.Windows.Forms.TextBox
        $t.Multiline = $true; $t.ReadOnly = $true; $t.ScrollBars = 'Both'; $t.WordWrap = $false
        $t.Font = New-Object System.Drawing.Font('Consolas', 9)
        $t.Text = $plan.Text.TrimEnd("`n").Replace("`n", "`r`n")
        $t.Location = New-Object System.Drawing.Point(12, 75); $t.Size = New-Object System.Drawing.Size(860, 390); $t.Anchor = $top -bor $left -bor $right -bor $bottom
        $d.Controls.Add($t)
        $ok = New-Object System.Windows.Forms.Button
        $ok.Text = $title; $ok.DialogResult = 'OK'; $ok.Location = New-Object System.Drawing.Point(560, 478); $ok.Size = New-Object System.Drawing.Size(200, 30); $ok.Anchor = $bottom -bor $right
        $cancel = New-Object System.Windows.Forms.Button
        $cancel.Text = 'Cancel'; $cancel.DialogResult = 'Cancel'; $cancel.Location = New-Object System.Drawing.Point(772, 478); $cancel.Size = New-Object System.Drawing.Size(100, 30); $cancel.Anchor = $bottom -bor $right
        $d.Controls.Add($ok); $d.Controls.Add($cancel)
        $d.AcceptButton = $cancel; $d.CancelButton = $cancel
        return $d.ShowDialog($form) -eq 'OK'
    }

    function Invoke-Plan($kind) {
        Start-Busy "plan: $kind"
        try {
            $f = Open-Folder
            $p = $f.MakePlan($kind)
            Show-Lines ($p.Text.TrimEnd("`n").Split("`n"))
            $say.Invoke("== plan SHA-256: $($p.Hash)")
        } catch { Show-Error (Get-ErrorText $_) } finally { Stop-Busy }
    }

    function Invoke-Change($kind, $title, $what) {
        Start-Busy $title
        try {
            $f = Open-Folder
            $p = $f.MakePlan($kind)
            Show-Lines ($p.Text.TrimEnd("`n").Split("`n"))
            if (-not $p.HasChanges) {
                if ($kind -eq 'undo') { $say.Invoke('== nothing to undo') } else { $say.Invoke('== nothing to change') }
                return
            }
            if ($kind -eq 'driver') {
                $sums = $f.CheckSums()
                if ($sums -ne 'ok' -and $sums -ne 'no SHA256SUMS') { throw "the package's EFI files differ from its SHA256SUMS (damaged or changed): nothing changed ($sums)" }
            }
            try { $f.CheckWritable() } catch {
                $text = Get-ErrorText $_
                if (-not $admin -and (Ask "$text`r`n`r`nRestart this tool as administrator?" 'Administrator rights')) { Restart-Elevated; return }
                throw
            }
            if (-not (Confirm-Plan $f $p $title $what)) { $say.Invoke('== cancelled, nothing changed'); return }
            $f.Run($kind, $p.Hash, $say)
        } catch { Show-Error (Get-ErrorText $_) } finally { Stop-Busy }
    }

    function Restart-Elevated {
        $exe = (Get-Process -Id $PID).Path
        $words = @('-NoProfile', '-ExecutionPolicy', 'Bypass')
        if ($PSVersionTable.PSEdition -ne 'Core') { $words += '-Sta' }
        $words += @('-File', (Format-Argument $PSCommandPath), '-Gui')
        foreach ($pair in @(@('-Efi', $efiBox.Text), @('-Package', $pkgBox.Text), @('-BackupDir', $bkBox.Text), @('-RulesFile', $RulesFile))) {
            if ($pair[1].Trim() -ne '') { $words += @($pair[0], (Format-Argument $pair[1])) }
        }
        if ($ssdtCheck.Checked -and $cardBox.Text.Trim() -ne '') { $words += @('-CardAcpiPath', (Format-Argument $cardBox.Text)) }
        try {
            Start-Process -FilePath $exe -ArgumentList ($words -join ' ') -Verb RunAs | Out-Null
            $form.Close()
        } catch { Show-Error "not restarted: $(Get-ErrorText $_)" }
    }

    $efiBrowse.Add_Click({
        $p = Get-Folder 'The EFI folder of your OpenCore EFI (it holds OC and BOOT). On a USB stick: E:\EFI; an EFI partition: mount it first.' $efiBox.Text
        if ($p) { $efiBox.Text = $p }
    })
    $pkgBrowse.Add_Click({
        $p = Get-Folder 'The unpacked intel-gpu-macos-driver-VERSION-efi.zip (it holds EFI\OC\Kexts\IntelXe.kext).' $pkgBox.Text
        if ($p) { $pkgBox.Text = $p }
    })
    $bkBrowse.Add_Click({
        $p = Get-Folder 'Where the backups of the EFI folder go (not inside the EFI folder).' $bkBox.Text
        if ($p) { $bkBox.Text = $p }
    })
    $detectButton.Add_Click({
        $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor
        try { $loc = Get-CardLocation } finally { $form.Cursor = [System.Windows.Forms.Cursors]::Default }
        if ($loc -eq '') {
            [void][System.Windows.Forms.MessageBox]::Show($form, "Windows does not show an Arc B580 with an ACPI Location path on this PC. If the card is in another PC, read the path there (Device Manager, see below).", 'IntelXe EFI')
            return
        }
        $path = [IntelXeEfi.EfiFolder]::ConvertLocationPath($loc, (Get-EfiRules).Int('cards.port_levels_above_gpu'))
        $cardBox.Text = $loc
        $say.Invoke("Windows: the B580's Location path is $loc")
        if ($path -eq 'none') { $say.Invoke("The card's upstream port has no ACPI name in it: the shipped table cannot be used (stage 4, step 2 of README-INSTALL.md).") }
        else { $say.Invoke("That is the ACPI path $path$(if ($path -eq $ShippedPath) { ', the shipped table''s' } else { ', not the shipped table''s: the table is adapted to it' }).") }
    })
    $ssdtCheck.Add_CheckedChanged({
        if ($ssdtCheck.Checked) {
            $ok = Ask ("SSDT-ARCOFF.aml hides the B580 from macOS's generic display driver, the one the macOS installer runs on. " +
                "Add it only after macOS is installed and install.sh --apply has run on it (stage 3); before that the screen stays black.`r`n`r`n" +
                "Is macOS installed and stage 3 done?") 'SSDT-ARCOFF.aml'
            if (-not $ok) { $ssdtCheck.Checked = $false }
        }
    })
    $statusButton.Add_Click({
        Start-Busy 'status'
        try { Show-Lines ((Open-Folder).Status()) } catch { Show-Error (Get-ErrorText $_) } finally { Stop-Busy }
    })
    $settingsPlan.Add_Click({ Invoke-Plan 'settings' })
    $driverPlan.Add_Click({ Invoke-Plan 'driver' })
    $undoPlan.Add_Click({ Invoke-Plan 'undo' })
    $settingsApply.Add_Click({ Invoke-Change 'settings' 'Change these settings' 'Stage 2: SIP, the boot-args and the quirks. The whole EFI folder is backed up first.' })
    $driverApply.Add_Click({ Invoke-Change 'driver' 'Make these changes' 'Stage 4: IntelXe.kext and its config.plist entry (SSDT-ARCOFF.aml when ticked, HybridPM.kext on a hybrid CPU). The whole EFI folder is backed up first.' })
    $undoApply.Add_Click({ Invoke-Change 'undo' 'Undo the last change' 'Puts back, from the newest backup, the files that change replaced. The backup stays.' })
    $adminButton.Add_Click({
        if (Ask 'Start this tool again as administrator (Windows asks for permission), with the folders filled in?' 'Administrator rights') { Restart-Elevated }
    })
    $mountButton.Add_Click({
        if (-not $admin) {
            if (Ask 'Mounting an EFI partition needs administrator rights. Restart this tool as administrator?' 'Administrator rights') { Restart-Elevated }
            return
        }
        $list = @()
        try { $list = @(Get-EspList) } catch { $say.Invoke("cannot list the EFI partitions: $(Get-ErrorText $_)") }
        $choice = $null
        if ($list.Count -gt 0) {
            $d = New-Object System.Windows.Forms.Form
            $d.Text = 'Mount an EFI partition'; $d.Size = New-Object System.Drawing.Size(760, 330); $d.StartPosition = 'CenterParent'
            $l = New-Object System.Windows.Forms.Label
            $l.Text = 'Choose the EFI partition that holds your OpenCore EFI (on the disk OpenCore boots from). Mounting writes nothing.'
            $l.Location = New-Object System.Drawing.Point(12, 10); $l.Size = New-Object System.Drawing.Size(720, 20)
            $d.Controls.Add($l)
            $lb = New-Object System.Windows.Forms.ListBox
            $lb.Location = New-Object System.Drawing.Point(12, 36); $lb.Size = New-Object System.Drawing.Size(720, 200)
            foreach ($e in $list) { [void]$lb.Items.Add((Format-Esp $e)) }
            $lb.SelectedIndex = 0
            $d.Controls.Add($lb)
            $ok = New-Object System.Windows.Forms.Button
            $ok.Text = 'Mount'; $ok.DialogResult = 'OK'; $ok.Location = New-Object System.Drawing.Point(520, 246); $ok.Size = New-Object System.Drawing.Size(100, 30)
            $cancel = New-Object System.Windows.Forms.Button
            $cancel.Text = 'Cancel'; $cancel.DialogResult = 'Cancel'; $cancel.Location = New-Object System.Drawing.Point(632, 246); $cancel.Size = New-Object System.Drawing.Size(100, 30)
            $d.Controls.Add($ok); $d.Controls.Add($cancel); $d.CancelButton = $cancel
            if ($d.ShowDialog($form) -ne 'OK' -or $lb.SelectedIndex -lt 0) { return }
            $choice = $list[$lb.SelectedIndex]
            if ($choice.Letter -ne '') { $efiBox.Text = "$($choice.Letter)\EFI"; $say.Invoke("already mounted at $($choice.Letter)"); return }
        } elseif (-not (Ask 'The EFI partitions could not be listed. Mount the one Windows booted from (mountvol /S)?' 'Mount an EFI partition')) { return }
        try {
            $letter = Get-FreeLetter
            $what = 'the EFI partition Windows booted from'
            $volume = 'system'
            if ($null -ne $choice) { $what = Format-Esp $choice; $volume = $choice.Volume }
            if (-not (Ask "Mount $what as $($letter)? Nothing is written to it. It stays mounted until you unmount it or restart." 'Mount an EFI partition')) { return }
            $letter = Mount-Esp $volume $letter
            $script:mounted = $letter
            $unmountButton.Enabled = $true
            $say.Invoke("== mounted at $letter")
            if (Test-Path -LiteralPath "$letter\EFI\OC\OpenCore.efi") { $efiBox.Text = "$letter\EFI"; $say.Invoke("$letter\EFI holds OpenCore") }
            else { $efiBox.Text = "$letter\"; $say.Invoke("no EFI\OC\OpenCore.efi on $($letter): not the OpenCore EFI? Unmount it and choose another") }
        } catch { Show-Error (Get-ErrorText $_) }
    })
    $unmountButton.Add_Click({
        if ($script:mounted -eq '') { return }
        try {
            Dismount-Esp $script:mounted
            $say.Invoke("== unmounted $($script:mounted)")
            $script:mounted = ''
            $unmountButton.Enabled = $false
        } catch { Show-Error (Get-ErrorText $_) }
    })

    $say.Invoke('Choose the EFI folder of your OpenCore EFI (on a USB stick, or the EFI partition: "Mount EFI partition..."),')
    $say.Invoke('then the stage: Settings (stage 2, any time) or Driver (stage 4, after install.sh --apply on macOS).')
    $say.Invoke('Each change shows its plan first, backs up the whole EFI folder, and can be undone one step at a time.')
    [void]$form.ShowDialog()
}

# ---- command line --------------------------------------------------------------------------------------------------
if ($MyInvocation.InvocationName -eq '.') { return }   # dot-sourced (the tests): the functions and the engine only

function Write-Line([string]$text) { Write-Host $text }
$sayHost = [Action[string]] { param($line) Write-Host $line }

$action = @()
if ($Status) { $action += 'status' }
if ($Plan) { $action += 'plan' }
if ($Apply) { $action += 'apply' }
if ($Undo) { $action += 'undo' }
if ($ListEsp) { $action += 'list' }
if ($MountEsp) { $action += 'mount' }
if ($UnmountEsp) { $action += 'unmount' }

if ($Help) { Get-Help -Detailed $PSCommandPath | Out-Host; exit 0 }
if ($action.Count -gt 1) { Write-Line "one of -Status, -Plan, -Apply, -Undo, -ListEsp, -MountEsp, -UnmountEsp at a time (-Help lists them)"; exit 2 }
if ($action.Count -eq 0) {
    if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne 'STA') {
        # Windows Forms needs a single-threaded apartment: start again with -Sta (once)
        if ($Gui) { Write-Line 'the window needs PowerShell in a single-threaded apartment: powershell.exe -Sta -File IntelXe-EFI.ps1'; exit 1 }
        $words = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Sta', '-File', (Format-Argument $PSCommandPath), '-Gui')
        foreach ($pair in @(@('-Efi', $Efi), @('-Package', $Package), @('-BackupDir', $BackupDir), @('-CardAcpiPath', $CardAcpiPath), @('-RulesFile', $RulesFile))) {
            if ($pair[1] -ne '') { $words += @($pair[0], (Format-Argument $pair[1])) }
        }
        Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList ($words -join ' ') | Out-Null
        exit 0
    }
    try { Show-Window } catch { Write-Line "the window failed: $(Get-ErrorText $_)"; exit 1 }
    exit 0
}

try {
    switch ($action[0]) {
        'list' {
            if (-not (Test-Admin)) { Write-Line 'listing the EFI partitions needs PowerShell started as administrator'; exit 1 }
            foreach ($e in @(Get-EspList)) { Write-Line "esp=$($e.Volume)|$(Format-Esp $e)" }
        }
        'mount' {
            $l = Mount-Esp $MountEsp $Letter
            Write-Line "== mounted at $l"
            if (Test-Path -LiteralPath "$l\EFI\OC\OpenCore.efi") { Write-Line "$l\EFI holds OpenCore: -Efi $l\EFI" } else { Write-Line "no EFI\OC\OpenCore.efi on $($l): not the OpenCore EFI?" }
        }
        'unmount' { Dismount-Esp $UnmountEsp; Write-Line "== unmounted $UnmountEsp" }
        default {
            if ($Efi -eq '') { Write-Line 'give the EFI folder: -Efi E:\EFI (-Help lists the options)'; exit 2 }
            $detected = ''
            $cpu = ''
            if ($action[0] -eq 'status' -or $Plan -eq 'driver' -or $Apply -eq 'driver') { $detected = Get-CardDetected; $cpu = Get-CpuName }
            $f = New-EfiFolder $Efi $Package $BackupDir $CardAcpiPath $detected $cpu
            switch ($action[0]) {
                'status' { foreach ($l in $f.Status()) { Write-Line $l } }
                'plan' {
                    $p = $f.MakePlan($Plan)
                    Write-Host -NoNewline $p.Text
                    Write-Line "== plan SHA-256: $($p.Hash)"
                    if (@($p.Lines | Where-Object { "$_" -like '*SSDT-ARCOFF*' }).Count -gt 0) {
                        Write-Line 'warning: SSDT-ARCOFF.aml hides the B580 from the display driver the macOS installer uses. Only after macOS is installed and install.sh --apply has run (stage 3).'
                    }
                    if ($p.HasChanges) {
                        $more = "-Efi $(Format-Argument $f.EfiDir)"
                        if ($Package -ne '') { $more += " -Package $(Format-Argument $Package)" }
                        if ($BackupDir -ne '') { $more += " -BackupDir $(Format-Argument $BackupDir)" }
                        if ($CardAcpiPath -ne '') { $more += " -CardAcpiPath $(Format-Argument $CardAcpiPath)" }
                        if ($RulesFile -ne '') { $more += " -RulesFile $(Format-Argument $RulesFile)" }
                        if ($Plan -eq 'undo') { Write-Line "to put it back: .\IntelXe-EFI.ps1 -Undo $more -Expect $($p.Hash)" }
                        else { Write-Line "to make these changes: .\IntelXe-EFI.ps1 -Apply $Plan $more -Expect $($p.Hash)" }
                    }
                }
                'apply' { $f.Run($Apply, $Expect, $sayHost) }
                'undo' { $f.Run('undo', $Expect, $sayHost) }
            }
        }
    }
} catch {
    Write-Line (Get-ErrorText $_)
    exit 1
}
exit 0
