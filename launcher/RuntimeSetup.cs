using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System;

namespace Burn3Launcher
{
	public static class RuntimeSetup
	{
		private sealed class CarPreviewWorker : IDisposable
		{
			private readonly string release;

			private readonly string root;

			private readonly Timer timer;

			private string processed;

			private int running;

			public CarPreviewWorker(string releaseDirectory, string installRoot)
			{
				release = releaseDirectory;
				root = installRoot;
				timer = new Timer(Check, null, 1000, 1000);
			}

			private void Check(object unused)
			{
				if (Interlocked.Exchange(ref running, 1) != 0)
				{
					return;
				}
				try
				{
					string text = Path.Combine(root, ".launcher", "car-thumbnails", "request.json");
					if (!File.Exists(text) || new FileInfo(text).Length > 65536)
					{
						return;
					}
					Dictionary<string, object> dictionary = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(text));
					if (dictionary != null && dictionary.ContainsKey("schema") && Convert.ToInt32(dictionary["schema"]) == 1 && dictionary.ContainsKey("request_id") && dictionary.ContainsKey("mods_directory"))
					{
						string text2 = dictionary["request_id"] as string;
						string text3 = dictionary["mods_directory"] as string;
						if (Regex.IsMatch(text2 ?? "", "^[a-zA-Z0-9_-]{1,96}$") && !string.IsNullOrEmpty(text3) && Path.IsPathRooted(text3) && !(text2 == processed))
						{
							PrepareCarThumbnails(release, root, Path.GetFullPath(text3), null);
							processed = text2;
						}
					}
				}
				catch (Exception)
				{
				}
				finally
				{
					Interlocked.Exchange(ref running, 0);
				}
			}

			public void Dispose()
			{
				timer.Dispose();
			}
		}

		private static string Quote(string value)
		{
			return "\"" + Regex.Replace(Regex.Replace(value, "(\\\\*)\"", "$1$1\\\""), "(\\\\+)$", "$1$1") + "\"";
		}

		public static void PrepareCarThumbnails(string release, string root, string modsDirectory, Action<string> progress)
		{
			if (string.IsNullOrEmpty(release))
			{
				return;
			}
			string text = Path.Combine(release, "setup", "python", "python.exe");
			string text2 = Path.Combine(release, "setup", "tools", "carmod", "paint_thumbnails.py");
			if (!File.Exists(text) || !File.Exists(text2))
			{
				return;
			}
			string text3 = Path.Combine(root, ".launcher", "logs");
			Directory.CreateDirectory(text3);
			ProcessStartInfo processStartInfo = new ProcessStartInfo(text, "-I " + Quote(text2) + " --install-root " + Quote(root) + " --mods-root " + Quote(modsDirectory ?? Path.Combine(root, "mods")));
			processStartInfo.UseShellExecute = false;
			processStartInfo.CreateNoWindow = true;
			processStartInfo.WorkingDirectory = root;
			processStartInfo.RedirectStandardOutput = true;
			processStartInfo.RedirectStandardError = true;
			ProcessStartInfo startInfo = processStartInfo;
			StreamWriter log = File.CreateText(Path.Combine(text3, "car-previews-" + Guid.NewGuid().ToString("N") + ".log"));
			try
			{
				Process process = new Process();
				process.StartInfo = startInfo;
				using (Process process2 = process)
				{
					DataReceivedEventHandler value = delegate(object sender, DataReceivedEventArgs e)
					{
						if (e.Data != null)
						{
							lock (log)
							{
								log.WriteLine(e.Data);
								log.Flush();
							}
						}
					};
					process2.OutputDataReceived += value;
					process2.ErrorDataReceived += value;
					if (progress != null)
					{
						progress("Preparing car paint previews...");
					}
					process2.Start();
					process2.PriorityClass = ProcessPriorityClass.BelowNormal;
					process2.BeginOutputReadLine();
					process2.BeginErrorReadLine();
					process2.WaitForExit();
					if (process2.ExitCode != 0)
					{
						log.WriteLine("Car previews could not be generated; default artwork remains available.");
					}
				}
			}
			finally
			{
				if (log != null)
				{
					((IDisposable)log).Dispose();
				}
			}
		}

		public static IDisposable WatchCarThumbnails(string release, string root)
		{
			return new CarPreviewWorker(release, root);
		}

		private static StreamWriter OpenSetupLog(string logs)
		{
			string text = Path.Combine(logs, "setup.log");
			if (File.Exists(text))
			{
				string path = "setup-" + File.GetLastWriteTimeUtc(text).ToString("yyyyMMdd-HHmmss-fff") + "-" + Guid.NewGuid().ToString("N") + ".log";
				File.Move(text, Path.Combine(logs, path));
			}
			return File.CreateText(text);
		}

		public static string Ensure(string release, string root, Action<string> progress)
		{
			string path = Path.Combine(release, "setup", "build.json");
			if (!File.Exists(path))
			{
				return Path.Combine(release, "bin", "takedown-recomp.exe");
			}
			JavaScriptSerializer javaScriptSerializer = new JavaScriptSerializer();
			Dictionary<string, object> dictionary = javaScriptSerializer.Deserialize<Dictionary<string, object>>(File.ReadAllText(path));
			string text = (string)dictionary["build_id"];
			if (!Regex.IsMatch(text, "^[a-f0-9]{64}$"))
			{
				throw new InvalidDataException("Invalid runtime build identity.");
			}
			string path2 = Path.Combine(root, ".launcher", "native", text);
			if (!Core.GameReady(root))
			{
				throw new InvalidDataException("Choose the supported Burnout 3 ISO before setting up the runtime.");
			}
			string text2 = Path.Combine(path2, "takedown-recomp.exe");
			string path3 = Path.Combine(path2, "ready.json");
			if (File.Exists(path3) && File.Exists(text2))
			{
				Dictionary<string, object> dictionary2 = javaScriptSerializer.Deserialize<Dictionary<string, object>>(File.ReadAllText(path3));
				if ((string)dictionary2["executable_sha256"] == Core.Hash(text2) && File.Exists(Path.Combine(root, "cache", "ui", "source-stamp.json")))
				{
					return text2;
				}
			}
			if (!Core.GameReady(root))
			{
				throw new InvalidDataException("Choose the supported Burnout 3 ISO before setting up the runtime.");
			}
			string text3 = Path.Combine(root, ".launcher", "logs");
			Directory.CreateDirectory(text3);
			ProcessStartInfo processStartInfo = new ProcessStartInfo(Path.Combine(release, "setup", "python", "python.exe"), "-I " + Quote(Path.Combine(release, "setup", "setup_runtime.py")) + " --release " + Quote(release) + " --install-root " + Quote(root));
			processStartInfo.UseShellExecute = false;
			processStartInfo.CreateNoWindow = true;
			processStartInfo.WorkingDirectory = root;
			processStartInfo.RedirectStandardOutput = true;
			processStartInfo.RedirectStandardError = true;
			ProcessStartInfo startInfo = processStartInfo;
			StreamWriter log = OpenSetupLog(text3);
			try
			{
				Process process = new Process();
				process.StartInfo = startInfo;
				using (Process process2 = process)
				{
					object gate = new object();
					DataReceivedEventHandler value = delegate(object sender, DataReceivedEventArgs e)
					{
						if (e.Data != null)
						{
							lock (gate)
							{
								log.WriteLine(e.Data);
								log.Flush();
							}
							if (progress != null)
							{
								progress((e.Data.Length > 140) ? e.Data.Substring(0, 140) : e.Data);
							}
						}
					};
					process2.OutputDataReceived += value;
					process2.ErrorDataReceived += value;
					process2.Start();
					process2.PriorityClass = ProcessPriorityClass.BelowNormal;
					process2.BeginOutputReadLine();
					process2.BeginErrorReadLine();
					process2.WaitForExit();
					if (process2.ExitCode != 0)
					{
						throw new InvalidOperationException("Game setup failed. Details are in .launcher/logs/setup.log. You can retry without importing the ISO again.");
					}
				}
			}
			finally
			{
				if (log != null)
				{
					((IDisposable)log).Dispose();
				}
			}
			if (!File.Exists(text2) || !File.Exists(path3))
			{
				throw new IOException("Game setup did not produce its executable.");
			}
			return text2;
		}
	}
}
