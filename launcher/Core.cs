using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Diagnostics;

namespace Burn3Launcher {
    public sealed class UpdateProgress {
        public string Component, Stage;
        public long Completed, Total;
        public double BytesPerSecond;
    }
    public sealed class Package {
        public string version { get; set; }
        public string url { get; set; }
        public string sha256 { get; set; }
        public long bytes { get; set; }
    }
    public sealed class Manifest {
        public int schema { get; set; }
        public Package launcher { get; set; }
        public Package runtime { get; set; }
    }
    public sealed class CatalogMod {
        public string id { get; set; }
        public string title { get; set; }
        public string author { get; set; }
        public string description { get; set; }
        public string version { get; set; }
        public string downloadUrl { get; set; }
        public string sha256 { get; set; }
        public long bytes { get; set; }
        public CatalogFile[] files { get; set; }
        public override string ToString() { return title + "  v" + version + "  by " + author; }
    }
    // A mod in the mods folder: a catalog mod with all of its downloaded
    // revisions, or one folder put there by hand.
    public sealed class LocalMod {
        public string Title { get; set; }
        public string Version { get; set; }
        public string CatalogId { get; set; }
        public List<string> Folders { get; set; }
        public override string ToString() {
            return Title + (String.IsNullOrEmpty(Version) ? "" : "  v" + Version) + (CatalogId != null ? "  (downloaded)" : "  (" + Folders[0] + ")");
        }
    }
    public sealed class CatalogFile { public string path { get; set; } public string sha256 { get; set; } public long size { get; set; } public bool directory { get; set; } }
    public sealed class ModCatalog { public int schema { get; set; } public CatalogMod[] mods { get; set; } }
    public sealed class CatalogRevision { public CatalogMod mod { get; set; } }
    public sealed class InstalledCatalog { public int schema { get; set; } public List<InstalledMod> mods { get; set; } public List<InstalledMod> revisions { get; set; } }
    public sealed class InstalledMod { public string id { get; set; } public string revision { get; set; } public string folder { get; set; } public string version { get; set; } }
    public sealed class ModSyncRequest { public int schema { get; set; } public string request_id { get; set; } public InstalledMod[] mods { get; set; } }
    // Launcher SETTINGS, kept per installation in .launcher/settings.json.
    // Burnout3.exe turns them into renderer environment switches at launch.
    public sealed class LaunchSettings {
        public int schema { get; set; }
        public bool gpu_crash_reports { get; set; }
        public bool descriptor_buffers { get; set; }
        // "progressive" (the game's 480p mode) or "interlaced" (its standard
        // 480i mode, for CRTs). Replaces the game's boot video-mode question.
        public string scan_mode { get; set; }
        public LaunchSettings() { schema = 1; descriptor_buffers = true; scan_mode = "progressive"; }
        public bool Interlaced { get { return scan_mode == "interlaced"; } }
        public static string PathFor(string root) { return Path.Combine(root, ".launcher", "settings.json"); }
        public static LaunchSettings Load(string root) {
            string path = PathFor(root);
            if (!File.Exists(path)) return new LaunchSettings();
            var settings = new JavaScriptSerializer().Deserialize<LaunchSettings>(File.ReadAllText(path));
            if (settings == null || settings.schema != 1) throw new InvalidDataException("Unsupported launcher settings file. Delete .launcher/settings.json to reset it.");
            if (settings.scan_mode != "interlaced") settings.scan_mode = "progressive";
            return settings;
        }
        public void Save(string root) { Core.AtomicText(PathFor(root), new JavaScriptSerializer().Serialize(this)); }
        public void ApplyToRenderer(System.Collections.Specialized.StringDictionary environment) {
            if (gpu_crash_reports) environment["GRANITE_VULKAN_POST_MORTEM"] = "1";
            if (!descriptor_buffers) environment["GRANITE_VULKAN_DESCRIPTOR_BUFFER"] = "0";
            environment["TAKEDOWN_SCAN_MODE"] = Interlaced ? "interlaced" : "progressive";
        }
    }
    public static class Core {
        public const string Version = "0.0.13";
        public const string Origin = "https://uwucammie.com";
        public const string ElfHash = "e118dba952bf9fde6a04ee284bb1f06fa0bb49c9e04bbe762af1039dcc443c66";
        static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16777216 };
        public static string Hash(string path) {
            return Hash(path, null);
        }
        public static string Hash(string path, Action<long, long> progress) {
            using (var file = File.OpenRead(path)) using (var sha = SHA256.Create()) {
                using (var stream = new CryptoStream(Stream.Null, sha, CryptoStreamMode.Write)) {
                    CopyBounded(file, stream, file.Length, progress == null ? (Action<long>)null : done => progress(done, file.Length));
                    stream.FlushFinalBlock();
                    return BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
                }
            }
        }
        sealed class ProgressSink {
            readonly string component;
            readonly Action<UpdateProgress> callback;
            readonly Stopwatch clock = Stopwatch.StartNew();
            long last = -100, began;
            string previous;
            public ProgressSink(string name, Action<UpdateProgress> action) { component = name; callback = action; }
            public void Report(string stage, long completed, long total) {
                if (callback == null) return;
                long now = clock.ElapsedMilliseconds;
                bool changed = previous != stage;
                if (changed) { previous = stage; began = now; }
                if (!changed && completed != total && now - last < 100) return;
                last = now;
                callback(new UpdateProgress { Component = component, Stage = stage, Completed = completed, Total = total,
                    BytesPerSecond = stage == "Downloading" && now > began ? completed * 1000.0 / (now - began) : 0 });
            }
        }
        public static Version ParseVersion(string value) {
            if (value == null || !System.Text.RegularExpressions.Regex.IsMatch(value, @"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"))
                throw new InvalidDataException("Invalid release version.");
            return new Version(value);
        }
        public static void ValidatePackage(Package p) {
            if (p == null) return;
            ParseVersion(p.version);
            Uri uri;
            if (!Uri.TryCreate(p.url, UriKind.Absolute, out uri) || uri.Scheme != "https" ||
                uri.Host != "uwucammie.com" || !uri.IsDefaultPort || uri.UserInfo != "" ||
                uri.Query != "" || uri.Fragment != "" || !uri.AbsolutePath.StartsWith("/burn3/downloads/", StringComparison.Ordinal))
                throw new InvalidDataException("Release URL is not an official download.");
            if (p.bytes < 1 || p.bytes > 2147483648L || p.sha256 == null ||
                !System.Text.RegularExpressions.Regex.IsMatch(p.sha256, "^[a-fA-F0-9]{64}$"))
                throw new InvalidDataException("Invalid release checksum or size.");
        }
        public static Manifest ReadManifest(string text) {
            var manifest = Json.Deserialize<Manifest>(text);
            if (manifest == null || manifest.schema != 1) throw new InvalidDataException("Unsupported release manifest.");
            ValidatePackage(manifest.launcher); ValidatePackage(manifest.runtime);
            return manifest;
        }
        static HttpWebResponse Request(string url) {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.AllowAutoRedirect = false;
            request.Timeout = 12000; request.ReadWriteTimeout = 15000;
            request.UserAgent = "Burn3Launcher/" + Version;
            var response = (HttpWebResponse)request.GetResponse();
            if (response.StatusCode != HttpStatusCode.OK) { response.Dispose(); throw new IOException("Download was redirected or unavailable."); }
            return response;
        }
        public static string FetchText(string path) {
            if (path != "/burn3/downloads/latest.json" && path != "/burn3/api/news" && path != "/burn3/api/mods" &&
                !System.Text.RegularExpressions.Regex.IsMatch(path, "^/burn3/api/mods/[a-z0-9][a-z0-9._-]{0,63}/revisions/[a-f0-9]{64}$")) throw new ArgumentException("Unknown endpoint.");
            using (var response = Request(Origin + path)) using (var input = response.GetResponseStream())
            using (var output = new MemoryStream()) {
                CopyBounded(input, output, path.StartsWith("/burn3/api/mods", StringComparison.Ordinal) ? 16777216 : 1048576);
                return Encoding.UTF8.GetString(output.ToArray());
            }
        }
        public static void ValidateMod(CatalogMod mod) {
            if (mod == null || !System.Text.RegularExpressions.Regex.IsMatch(mod.id ?? "", "^[a-z0-9][a-z0-9._-]{0,63}$") || mod.id.Contains("..") || mod.id.EndsWith("."))
                throw new InvalidDataException("Invalid mod identity.");
            SafePath(Path.GetTempPath(), mod.id);
            if (mod.version == null || mod.version.Length > 96 || !System.Text.RegularExpressions.Regex.IsMatch(mod.version, @"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")) throw new InvalidDataException("Invalid mod version.");
            if (!System.Text.RegularExpressions.Regex.IsMatch(mod.sha256 ?? "", "^[a-f0-9]{64}$") || mod.bytes < 1 || mod.bytes > 536870912L)
                throw new InvalidDataException("Invalid mod checksum or size.");
            string expected = Origin + "/burn3/api/mods/" + mod.id + "/revisions/" + mod.sha256 + "/download";
            if (!String.Equals(mod.downloadUrl, expected, StringComparison.Ordinal)) throw new InvalidDataException("Mod download is not an official immutable revision.");
            if (String.IsNullOrWhiteSpace(mod.title) || mod.title.Length > 200 || (mod.description ?? "").Length > 16000 || (mod.author ?? "").Length > 2048)
                throw new InvalidDataException("Invalid mod catalog description.");
        }
        public static ModCatalog ReadModCatalog(string text) {
            var catalog = Json.Deserialize<ModCatalog>(text);
            if (catalog == null || catalog.schema != 1 || catalog.mods == null || catalog.mods.Length > 10000) throw new InvalidDataException("Unsupported mod catalog.");
            var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var mod in catalog.mods) { ValidateMod(mod); if (!ids.Add(mod.id)) throw new InvalidDataException("Duplicate catalog mod."); }
            return catalog;
        }
        public static string ModRevisionFolder(CatalogMod mod) { ValidateMod(mod); return "catalog-" + mod.id + "-" + mod.sha256.Substring(0, 12); }
        static InstalledCatalog InstalledMods(string root) {
            string record = Path.Combine(root, "mods", ".catalog.json");
            if (!File.Exists(record)) return new InstalledCatalog { schema = 1, mods = new List<InstalledMod>(), revisions = new List<InstalledMod>() };
            var saved = Json.Deserialize<InstalledCatalog>(File.ReadAllText(record));
            if (saved == null || saved.schema != 1 || saved.mods == null) throw new InvalidDataException("Invalid installed mod catalog.");
            if (saved.revisions == null) saved.revisions = new List<InstalledMod>(saved.mods);
            return saved;
        }
        public static string InstalledModVersion(string root, CatalogMod mod) {
            ValidateMod(mod);
            foreach (var saved in InstalledMods(root).mods) if (saved.id == mod.id) return saved.revision == mod.sha256 ? mod.version : saved.version;
            return null;
        }
        public static string InstallMod(string root, CatalogMod mod, Action<UpdateProgress> progress) {
            ValidateMod(mod);
            string store = Path.Combine(root, ".launcher"); Directory.CreateDirectory(store);
            RejectReparsePoint(store);
            string folder = ModRevisionFolder(mod), destination = Path.Combine(root, "mods", folder);
            if (Directory.Exists(destination) && mod.files != null && mod.files.Length > 0) {
                RejectReparsePoint(Path.Combine(root, "mods"));
                using (var held = new FileStream(Path.Combine(store, "mods.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
                    var reuse = new ProgressSink(mod.title, progress);
                    reuse.Report("Verifying installed", 0, mod.files.Length);
                    if (VerifyModInventory(destination, mod.files, done => reuse.Report("Verifying installed", done, mod.files.Length))) {
                        SelectMod(root, mod, folder); reuse.Report("Complete", 1, 1); return folder;
                    }
                }
            }
            string zip = Path.Combine(store, "mod-" + Guid.NewGuid().ToString("N") + ".zip");
            var sink = new ProgressSink(mod.title, progress);
            try {
                using (var response = Request(mod.downloadUrl)) using (var input = response.GetResponseStream())
                using (var output = new FileStream(zip, FileMode.CreateNew)) CopyBounded(input, output, mod.bytes, done => sink.Report("Downloading", done, mod.bytes));
                return InstallModArchive(root, mod, zip, progress);
            } finally { if (File.Exists(zip)) File.Delete(zip); }
        }
        public static ModSyncRequest ReadModSyncRequest(string path) {
            if (new FileInfo(path).Length > 1048576) throw new InvalidDataException("Mod synchronization request is too large.");
            var request = Json.Deserialize<ModSyncRequest>(File.ReadAllText(path));
            if (request == null || request.schema != 1 || !System.Text.RegularExpressions.Regex.IsMatch(request.request_id ?? "", "^[a-zA-Z0-9_-]{1,96}$") || request.mods == null || request.mods.Length > 100)
                throw new InvalidDataException("Invalid mod synchronization request.");
            var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var mod in request.mods) {
                if (mod == null || !System.Text.RegularExpressions.Regex.IsMatch(mod.id ?? "", "^[a-z0-9][a-z0-9._-]{0,63}$") || mod.id.Contains("..") || mod.id.EndsWith(".") ||
                    !System.Text.RegularExpressions.Regex.IsMatch(mod.revision ?? "", "^[a-f0-9]{64}$") || !ids.Add(mod.id)) throw new InvalidDataException("Invalid requested mod revision.");
                SafePath(Path.GetTempPath(), mod.id);
            }
            return request;
        }
        public static void SynchronizeMods(string root, ModSyncRequest request, Action<UpdateProgress> progress) {
            var installed = new List<InstalledMod>(); string error = null;
            try {
                foreach (var wanted in request.mods) {
                    var response = Json.Deserialize<CatalogRevision>(FetchText("/burn3/api/mods/" + wanted.id + "/revisions/" + wanted.revision));
                    var mod = response == null ? null : response.mod;
                    ValidateMod(mod);
                    if (mod.id != wanted.id || mod.sha256 != wanted.revision) throw new InvalidDataException("Catalog did not return the requested exact revision.");
                    string folder = InstallMod(root, mod, progress);
                    installed.Add(new InstalledMod { id = mod.id, revision = mod.sha256, folder = folder, version = mod.version });
                }
            } catch (Exception e) { error = e.Message; }
            AtomicText(Path.Combine(root, ".launcher", "mod-sync", "result.json"), Json.Serialize(new { schema = 1, request_id = request.request_id, success = error == null, error = error, mods = installed }));
            if (error != null) throw new IOException(error);
        }
        public static string InstallModArchive(string root, CatalogMod mod, string zip, Action<UpdateProgress> progress) {
            ValidateMod(mod);
            string store = Path.Combine(root, ".launcher"); Directory.CreateDirectory(store);
            RejectReparsePoint(store);
            var sink = new ProgressSink(mod.title, progress);
            using (var held = new FileStream(Path.Combine(store, "mods.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
                if (new FileInfo(zip).Length != mod.bytes || !Hash(zip, (done, total) => sink.Report("Verifying", done, total)).Equals(mod.sha256, StringComparison.Ordinal))
                    throw new InvalidDataException("Mod checksum did not match. Your installed mods are unchanged.");
                string staging = Path.Combine(store, "mod-stage-" + Guid.NewGuid().ToString("N"));
                try {
                    // Tighter mod bounds are checked before the generic safe extractor writes anything.
                    using (var archive = ZipFile.OpenRead(zip)) {
                        long total = 0;
                        if (archive.Entries.Count > 10000) throw new InvalidDataException("Too many mod files.");
                        foreach (var entry in archive.Entries) { total += entry.Length; if (total > 2147483648L) throw new InvalidDataException("Mod exceeds extracted size limit."); }
                    }
                    ExtractPackage(zip, staging, (done, total) => sink.Report("Installing", done, total));
                    string manifest = Path.Combine(staging, "mod.json");
                    if (!File.Exists(manifest) || new FileInfo(manifest).Length > 1048576) throw new InvalidDataException("Mod package needs mod.json at its root.");
                    var data = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(manifest));
                    if (data == null || !data.ContainsKey("id") || !data.ContainsKey("version") || (string)data["id"] != mod.id || (string)data["version"] != mod.version)
                        throw new InvalidDataException("Mod manifest does not match the catalog.");
                    string mods = Path.Combine(root, "mods"), folder = ModRevisionFolder(mod);
                    Directory.CreateDirectory(mods);
                    RejectReparsePoint(mods);
                    string destination = Path.Combine(mods, folder);
                    // Revisions are immutable: even a running game keeps all of its old paths.
                    if (Directory.Exists(destination)) {
                        RejectReparseTree(destination);
                        var stagedFiles = Directory.GetFiles(staging, "*", SearchOption.AllDirectories);
                        if (stagedFiles.Length != Directory.GetFiles(destination, "*", SearchOption.AllDirectories).Length) throw new InvalidDataException("Existing mod revision was modified; preserve or move that folder before reinstalling.");
                        var directories = Directory.GetDirectories(staging, "*", SearchOption.AllDirectories);
                        if (directories.Length != Directory.GetDirectories(destination, "*", SearchOption.AllDirectories).Length) throw new InvalidDataException("Existing mod revision directories were modified.");
                        foreach (string directory in directories) if (!Directory.Exists(Path.Combine(destination, directory.Substring(staging.Length + 1)))) throw new InvalidDataException("Existing mod revision directories were modified.");
                        foreach (string file in stagedFiles) {
                            string existing = Path.Combine(destination, file.Substring(staging.Length + 1));
                            if (!File.Exists(existing) || (File.GetAttributes(existing) & FileAttributes.ReparsePoint) != 0 || Hash(existing) != Hash(file)) throw new InvalidDataException("Existing mod revision was modified; preserve or move that folder before reinstalling.");
                        }
                    } else Directory.Move(staging, destination);
                    SelectMod(root, mod, folder);
                    sink.Report("Complete", 1, 1);
                    return folder;
                } finally { if (Directory.Exists(staging)) Directory.Delete(staging, true); }
            }
        }
        static void SelectMod(string root, CatalogMod mod, string folder) {
            var selected = InstalledMods(root);
            selected.mods.RemoveAll(saved => saved.id == mod.id);
            var installed = new InstalledMod { id = mod.id, version = mod.version, revision = mod.sha256, folder = folder };
            selected.mods.Add(installed);
            selected.revisions.RemoveAll(saved => saved.id == mod.id && saved.revision == mod.sha256);
            selected.revisions.Add(installed);
            AtomicText(Path.Combine(root, "mods", ".catalog.json"), Json.Serialize(selected));
        }
        static string ModManifestField(string folder, string key) {
            string manifest = Path.Combine(folder, "mod.json");
            if (File.Exists(manifest) && new FileInfo(manifest).Length <= 1048576) {
                try {
                    var data = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(manifest));
                    object value;
                    if (data != null && data.TryGetValue(key, out value) && value is string && ((string)value).Length > 0) return (string)value;
                } catch (ArgumentException) { return null; } // malformed manifest: the folder name is shown
            }
            return null;
        }
        // Every mod in the mods folder, catalog revisions grouped by mod.
        public static List<LocalMod> ListLocalMods(string root) {
            var result = new List<LocalMod>();
            string mods = Path.Combine(root, "mods");
            if (!Directory.Exists(mods)) return result;
            var catalog = InstalledMods(root);
            var byId = new Dictionary<string, LocalMod>(StringComparer.OrdinalIgnoreCase);
            var catalogFolders = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var saved in catalog.revisions) if (saved.folder != null) catalogFolders[saved.folder] = saved.id;
            foreach (var saved in catalog.mods) if (saved.folder != null) catalogFolders[saved.folder] = saved.id;
            foreach (string path in Directory.GetDirectories(mods)) {
                string folder = Path.GetFileName(path);
                string id;
                if (catalogFolders.TryGetValue(folder, out id)) {
                    LocalMod mod;
                    if (!byId.TryGetValue(id, out mod)) {
                        mod = new LocalMod { CatalogId = id, Folders = new List<string>(), Title = id };
                        byId[id] = mod; result.Add(mod);
                    }
                    mod.Folders.Add(folder);
                    var current = catalog.mods.Find(saved => saved.id == id);
                    if (current != null && String.Equals(current.folder, folder, StringComparison.OrdinalIgnoreCase)) {
                        mod.Version = current.version;
                        mod.Title = ModManifestField(path, "name") ?? id;
                    }
                } else {
                    result.Add(new LocalMod { Title = ModManifestField(path, "name") ?? folder, Version = ModManifestField(path, "version"),
                                              Folders = new List<string> { folder } });
                }
            }
            result.Sort((a, b) => String.Compare(a.Title, b.Title, StringComparison.OrdinalIgnoreCase));
            return result;
        }
        // Deletes a mod's folders and forgets it in .catalog.json and
        // load_order.txt. Never call while the game runs: it reads these folders.
        public static void RemoveLocalMod(string root, LocalMod mod) {
            string store = Path.Combine(root, ".launcher"); Directory.CreateDirectory(store);
            RejectReparsePoint(store);
            string mods = Path.Combine(root, "mods");
            RejectReparsePoint(mods);
            using (var held = new FileStream(Path.Combine(store, "mods.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
                var folders = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (string folder in mod.Folders) {
                    string path = SafePath(mods, folder);
                    if (!String.Equals(Path.GetDirectoryName(path), Path.GetFullPath(mods).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("A mod folder must be directly inside the mods folder.");
                    if (Directory.Exists(path)) { RejectReparseTree(path); Directory.Delete(path, true); }
                    folders.Add(folder);
                }
                if (mod.CatalogId != null && File.Exists(Path.Combine(mods, ".catalog.json"))) {
                    var catalog = InstalledMods(root);
                    catalog.mods.RemoveAll(saved => saved.id == mod.CatalogId);
                    catalog.revisions.RemoveAll(saved => saved.id == mod.CatalogId);
                    AtomicText(Path.Combine(mods, ".catalog.json"), Json.Serialize(catalog));
                }
                string order = Path.Combine(mods, "load_order.txt");
                if (File.Exists(order)) {
                    var kept = new List<string>();
                    foreach (string line in File.ReadAllLines(order)) if (!folders.Contains(line.Trim())) kept.Add(line);
                    AtomicText(order, String.Join("\n", kept) + "\n");
                }
            }
        }
        public static bool VerifyModInventory(string destination, CatalogFile[] files, Action<long> progress) {
            if (files == null || files.Length < 1 || files.Length > 20000) return false;
            RejectReparseTree(destination);
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            long complete = 0, total = 0;
            foreach (var file in files) {
                if (file == null) throw new InvalidDataException("Invalid catalog inventory.");
                string path = SafePath(destination, file.path);
                if (!paths.Add(path)) throw new InvalidDataException("Duplicate catalog inventory path.");
                if (file.directory) { if (!Directory.Exists(path)) return false; }
                else {
                    if (file.size < 0 || file.size > 2147483648L || !System.Text.RegularExpressions.Regex.IsMatch(file.sha256 ?? "", "^[a-f0-9]{64}$")) throw new InvalidDataException("Invalid catalog file hash or size.");
                    total += file.size;
                    if (total > 2147483648L) throw new InvalidDataException("Catalog inventory exceeds its size limit.");
                    if (!File.Exists(path) || new FileInfo(path).Length != file.size || Hash(path) != file.sha256) return false;
                }
                if (progress != null) progress(++complete);
            }
            return paths.Contains(SafePath(destination, "mod.json")) && File.Exists(Path.Combine(destination, "mod.json")) &&
                paths.Count == Directory.GetFiles(destination, "*", SearchOption.AllDirectories).Length + Directory.GetDirectories(destination, "*", SearchOption.AllDirectories).Length;
        }
        static void RejectReparsePoint(string path) {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Managed mod paths cannot be symbolic links or junctions.");
        }
        static void RejectReparseTree(string path) {
            RejectReparsePoint(path);
            foreach (string file in Directory.GetFiles(path)) RejectReparsePoint(file);
            foreach (string directory in Directory.GetDirectories(path)) RejectReparseTree(directory);
        }
        public static void CopyBounded(Stream input, Stream output, long maximum, Action<long> progress = null) {
            var buffer = new byte[131072]; long total = 0; int count;
            if (progress != null) progress(0);
            while ((count = input.Read(buffer, 0, buffer.Length)) != 0) {
                total += count;
                if (total > maximum) throw new InvalidDataException("Download or extracted data exceeds its limit.");
                output.Write(buffer, 0, count);
                if (progress != null) progress(total);
            }
            if (progress != null) progress(total);
        }
        // Windows filenames are checked before any extraction, including device
        // names, alternate streams and case-insensitive aliases.
        public static string SafePath(string root, string name) {
            if (String.IsNullOrEmpty(name) || name.Contains("\\") || name.StartsWith("/")) throw new InvalidDataException("Invalid package path.");
            foreach (string part in name.TrimEnd('/').Split('/')) {
                if (part == "" || part == "." || part == ".." || part.EndsWith(".") || part.EndsWith(" ") ||
                    part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                    System.Text.RegularExpressions.Regex.IsMatch(part, @"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])($|\.)", System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                    throw new InvalidDataException("Unsafe package path.");
            }
            string prefix = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            string result = Path.GetFullPath(Path.Combine(prefix, name.Replace('/', Path.DirectorySeparatorChar)));
            if (!result.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Path escapes installation directory.");
            return result;
        }
        public static void ExtractPackage(string zip, string destination) {
            ExtractPackage(zip, destination, null);
        }
        public static void ExtractPackage(string zip, string destination, Action<long, long> progress) {
            if (Directory.Exists(destination)) throw new IOException("Staging directory already exists.");
            using (var archive = ZipFile.OpenRead(zip)) {
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase); long total = 0;
                if (archive.Entries.Count > 50000) throw new InvalidDataException("Too many package files.");
                foreach (var entry in archive.Entries) {
                    string path = SafePath(destination, entry.FullName);
                    if (!names.Add(path) || ((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000 || (entry.ExternalAttributes & 0x400) != 0)
                        throw new InvalidDataException("Duplicate path or symbolic link in package.");
                    total += entry.Length;
                    if (total > 8589934592L) throw new InvalidDataException("Package exceeds extracted size limit.");
                }
                Directory.CreateDirectory(destination);
                long extracted = 0;
                if (progress != null) progress(0, total);
                foreach (var entry in archive.Entries) {
                    var path = SafePath(destination, entry.FullName);
                    if (entry.FullName.EndsWith("/")) { Directory.CreateDirectory(path); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    using (var input = entry.Open()) using (var output = new FileStream(path, FileMode.CreateNew)) {
                        CopyBounded(input, output, entry.Length, progress == null ? (Action<long>)null : done => progress(extracted + done, total));
                        if (output.Length != entry.Length) throw new InvalidDataException("Truncated package entry.");
                    }
                    extracted += entry.Length;
                }
                if (progress != null) progress(extracted, total);
            }
        }
        // Removes what earlier updates left behind: every runtime and launcher
        // folder but the active ones (and `keepLauncher`, the folder of the
        // launcher that is running), and every built game but the active
        // runtime's. A folder is renamed before it is deleted, so one whose
        // files are in use is left whole and tried again next time. Never call
        // while the game runs. Returns the bytes freed; `failures` gets one line
        // per folder that could not be removed.
        public static long PruneOldReleases(string root, string keepLauncher, List<string> failures) {
            string store = Path.Combine(root, ".launcher");
            if (!Directory.Exists(store)) return 0;
            RejectReparsePoint(store);
            var keep = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string activeRuntime = ActiveDirectory(store, "runtime"), activeLauncher = ActiveDirectory(store, "launcher");
            if (activeRuntime == null) return 0;
            keep.Add(activeRuntime);
            if (activeLauncher != null) keep.Add(activeLauncher);
            if (!String.IsNullOrEmpty(keepLauncher)) keep.Add(Path.GetFileName(Path.GetFullPath(keepLauncher).TrimEnd(Path.DirectorySeparatorChar)));
            var doomed = new List<string>();
            foreach (string path in Directory.GetDirectories(store)) {
                string name = Path.GetFileName(path);
                if (keep.Contains(name)) continue;
                if (System.Text.RegularExpressions.Regex.IsMatch(name, @"^(runtime|launcher)-[0-9]+\.[0-9]+\.[0-9]+-[a-f0-9]{32}$") ||
                    System.Text.RegularExpressions.Regex.IsMatch(name, @"\.old-[a-f0-9]{32}$"))
                    doomed.Add(path);
            }
            string build = ActiveBuildId(Path.Combine(store, activeRuntime));
            string native = Path.Combine(store, "native");
            if (build != null && Directory.Exists(native)) {
                RejectReparsePoint(native);
                foreach (string path in Directory.GetDirectories(native)) {
                    string name = Path.GetFileName(path);
                    if (name != build && System.Text.RegularExpressions.Regex.IsMatch(name, "^[a-f0-9]{64}(\\.old-[a-f0-9]{32})?$")) doomed.Add(path);
                }
            }
            long freed = 0;
            foreach (string path in doomed) {
                try {
                    RejectReparseTree(path);
                    long size = 0;
                    foreach (string file in Directory.GetFiles(path, "*", SearchOption.AllDirectories)) size += new FileInfo(file).Length;
                    string moved = path;
                    if (!System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(path), @"\.old-[a-f0-9]{32}$")) {
                        moved = path + ".old-" + Guid.NewGuid().ToString("N");
                        Directory.Move(path, moved);
                    }
                    Directory.Delete(moved, true);
                    freed += size;
                } catch (Exception e) {
                    if (e is IOException || e is UnauthorizedAccessException || e is InvalidDataException) failures.Add(Path.GetFileName(path) + ": " + e.Message);
                    else throw;
                }
            }
            return freed;
        }
        static string ActiveDirectory(string store, string component) {
            string record = Path.Combine(store, component + ".json");
            if (!File.Exists(record)) return null;
            var active = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(record));
            object directory;
            if (active == null || !active.TryGetValue("directory", out directory) || !(directory is string) ||
                !System.Text.RegularExpressions.Regex.IsMatch((string)directory, "^" + component + @"-[0-9]+\.[0-9]+\.[0-9]+-[a-f0-9]{32}$"))
                throw new InvalidDataException("Invalid installed release record.");
            return (string)directory;
        }
        static string ActiveBuildId(string release) {
            string manifest = Path.Combine(release, "setup", "build.json");
            if (!File.Exists(manifest)) return null;
            var data = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(manifest));
            object id;
            if (data == null || !data.TryGetValue("build_id", out id) || !(id is string) ||
                !System.Text.RegularExpressions.Regex.IsMatch((string)id, "^[a-f0-9]{64}$")) return null;
            return (string)id;
        }
        public static void AtomicText(string path, string text) {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllText(temporary, text, new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temporary, path, null); else File.Move(temporary, path);
        }
        public static void ValidateWindowsExecutable(string path) {
            using (var file = File.OpenRead(path)) using (var reader = new BinaryReader(file)) {
                if (file.Length < 128 || reader.ReadUInt16() != 0x5a4d) throw new InvalidDataException("Release entry point is not a Windows executable.");
                file.Position = 60; uint header = reader.ReadUInt32();
                if (header > file.Length - 94) throw new InvalidDataException("Invalid executable header.");
                file.Position = header;
                if (reader.ReadUInt32() != 0x4550 || reader.ReadUInt16() != 0x8664) throw new InvalidDataException("Release entry point must be a 64-bit Windows executable.");
                file.Position = header + 24 + 68;
                if (reader.ReadUInt16() != 2) throw new InvalidDataException("Release entry point must use the Windows GUI subsystem.");
            }
        }
        public static string Install(string root, string component, Package package) { return Install(root, component, package, null); }
        public static string Install(string root, string component, Package package, Action<UpdateProgress> progress) {
            ValidatePackage(package);
            if (package == null || (component != "launcher" && component != "runtime")) throw new ArgumentException("Invalid component.");
            string store = Path.Combine(root, ".launcher"); Directory.CreateDirectory(store);
            string zip = Path.Combine(store, "download-" + Guid.NewGuid().ToString("N") + ".zip");
            var sink = new ProgressSink(component, progress);
            sink.Report("Downloading", 0, package.bytes);
            try {
                using (var response = Request(package.url)) using (var input = response.GetResponseStream())
                using (var output = new FileStream(zip, FileMode.CreateNew)) CopyBounded(input, output, package.bytes, done => sink.Report("Downloading", done, package.bytes));
                return InstallArchive(root, component, package, zip, progress);
            } finally { if (File.Exists(zip)) File.Delete(zip); }
        }
        public static string InstallArchive(string root, string component, Package package, string zip) { return InstallArchive(root, component, package, zip, null); }
        public static string InstallArchive(string root, string component, Package package, string zip, Action<UpdateProgress> progress) {
            ValidatePackage(package);
            if (package == null || (component != "launcher" && component != "runtime")) throw new ArgumentException("Invalid component.");
            string store = Path.Combine(root, ".launcher"); Directory.CreateDirectory(store);
            var sink = new ProgressSink(component, progress);
            using (var held = new FileStream(Path.Combine(store, "install.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
                string record = Path.Combine(store, component + ".json");
                if (File.Exists(record)) {
                    var previous = Json.Deserialize<Dictionary<string, string>>(File.ReadAllText(record));
                    if (ParseVersion(package.version) <= ParseVersion(previous["version"])) throw new InvalidDataException("This version is already installed or older.");
                }
                if (new FileInfo(zip).Length != package.bytes || !Hash(zip, (done, total) => sink.Report("Verifying", done, total)).Equals(package.sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Release checksum did not match. The installed version is unchanged.");
                string staging = Path.Combine(store, "staging-" + Guid.NewGuid().ToString("N"));
                try {
                    ExtractPackage(zip, staging, (done, total) => sink.Report("Installing", done, total));
                    sink.Report("Activating", 0, 0);
                    string entry = component == "launcher" ? "Burnout3Launcher.exe" : "Burnout3.exe";
                    if (!File.Exists(Path.Combine(staging, entry)))
                        throw new InvalidDataException("Release is missing its entry point.");
                    ValidateWindowsExecutable(Path.Combine(staging, entry));
                    if (!File.Exists(Path.Combine(staging, "VERSION")) || File.ReadAllText(Path.Combine(staging, "VERSION")).Trim() != package.version)
                        throw new InvalidDataException("Release version does not match the manifest.");
                    string relative = component + "-" + package.version + "-" + Guid.NewGuid().ToString("N");
                    Directory.Move(staging, Path.Combine(store, relative));
                    AtomicText(record, Json.Serialize(new { version = package.version, directory = relative }));
                    sink.Report("Complete", 1, 1);
                    return package.version;
                } finally { if (Directory.Exists(staging)) Directory.Delete(staging, true); }
            }
        }
        public static bool GameReady(string root) {
            string elf = Path.Combine(root, "game", "SLUS_210.50");
            return File.Exists(elf) && Hash(elf) == ElfHash;
        }
        // Read ISO9660 directly. No mount, administrator rights or external
        // extractor is required. Only the supported retail executable is admitted.
        public static void ImportIso(string iso, string root) {
            string target = Path.Combine(root, "game");
            if (Directory.Exists(target)) throw new IOException("A game folder already exists. Your installed game has been preserved.");
            string stage = Path.Combine(root, ".launcher", "iso-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(stage);
            try {
            using (var file = File.OpenRead(iso)) {
                byte[] descriptor = new byte[2048]; bool found = false;
                for (int sector = 16; sector < 80; sector++) {
                    ReadAt(file, sector * 2048L, descriptor);
                    if (Encoding.ASCII.GetString(descriptor, 1, 5) != "CD001" || descriptor[6] != 1) throw new InvalidDataException("Not a supported ISO9660 disc image.");
                    if (descriptor[0] == 255) break;
                    if (descriptor[0] == 1) { found = true; break; }
                }
                if (!found || BitConverter.ToUInt16(descriptor, 128) != 2048) throw new InvalidDataException("Unsupported disc sector layout.");
                var visited = new HashSet<uint>(); int entries = 0; long extracted = 0;
                ExtractDirectory(file, BitConverter.ToUInt32(descriptor, 158), BitConverter.ToUInt32(descriptor, 166), stage, 0, visited, ref entries, ref extracted);
            }
            string elf = Path.Combine(stage, "SLUS_210.50");
            if (!File.Exists(elf) || Hash(elf) != ElfHash || !File.Exists(Path.Combine(stage, "SYSTEM.CNF")))
                throw new InvalidDataException("This disc revision is unsupported. Use the original US SLUS-21050 disc.");
            Directory.Move(stage, target);
            } finally { if (Directory.Exists(stage)) Directory.Delete(stage, true); }
        }
        static void ReadAt(FileStream stream, long offset, byte[] data) {
            if (offset < 0 || offset > stream.Length - data.Length) throw new InvalidDataException("Disc extent is outside the image.");
            stream.Position = offset; int used = 0, n;
            while (used < data.Length && (n = stream.Read(data, used, data.Length - used)) > 0) used += n;
            if (used != data.Length) throw new EndOfStreamException();
        }
        static void ExtractDirectory(FileStream file, uint sector, uint size, string destination, int depth, HashSet<uint> visited, ref int entries, ref long extracted) {
            if (depth > 16 || size > 16777216 || !visited.Add(sector)) throw new InvalidDataException("Invalid disc directory tree.");
            byte[] data = new byte[size]; ReadAt(file, sector * 2048L, data);
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (int offset = 0; offset < data.Length;) {
                int length = data[offset];
                if (length == 0) { offset = ((offset / 2048) + 1) * 2048; continue; }
                if (length < 34 || offset + length > data.Length || offset % 2048 + length > 2048 || data[offset + 32] > length - 33)
                    throw new InvalidDataException("Invalid disc directory record.");
                uint lba = BitConverter.ToUInt32(data, offset + 2), bytes = BitConverter.ToUInt32(data, offset + 10);
                byte flags = data[offset + 25];
                string name = Encoding.ASCII.GetString(data, offset + 33, data[offset + 32]).Split(';')[0];
                offset += length;
                if (name == "\0" || name == "\x01") continue;
                if (++entries > 50000 || (flags & 0x80) != 0) throw new InvalidDataException("Unsupported multi-extent or oversized disc.");
                string path = SafePath(destination, name);
                if (!names.Add(path)) throw new InvalidDataException("Duplicate disc path.");
                if ((flags & 2) != 0) { Directory.CreateDirectory(path); ExtractDirectory(file, lba, bytes, path, depth + 1, visited, ref entries, ref extracted); }
                else {
                    extracted += bytes;
                    if (extracted > 8589934592L || lba * 2048L > file.Length - bytes) throw new InvalidDataException("Invalid disc file extent.");
                    file.Position = lba * 2048L;
                    using (var output = new FileStream(path, FileMode.CreateNew)) {
                        byte[] buffer = new byte[131072]; long remaining = bytes;
                        while (remaining > 0) {
                            int n = file.Read(buffer, 0, (int)Math.Min(remaining, buffer.Length));
                            if (n == 0) throw new EndOfStreamException();
                            output.Write(buffer, 0, n); remaining -= n;
                        }
                    }
                }
            }
        }
    }
}
