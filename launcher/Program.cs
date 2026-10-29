using Microsoft.Win32;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media.Imaging;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using System.Windows;
using System;
using Path = System.IO.Path;

[assembly: AssemblyFileVersion("0.0.13.0")]
[assembly: AssemblyTitle("Burnout 3 Launcher")]
[assembly: AssemblyProduct("Burnout 3 Launcher")]
[assembly: AssemblyCompany("uwucammie")]
[assembly: AssemblyVersion("0.0.13.0")]

namespace Burn3Launcher
{
	public static class Program
	{
		public static string Quote(string value)
		{
			return "\"" + Regex.Replace(Regex.Replace(value, "(\\\\*)\"", "$1$1\\\""), "(\\\\+)$", "$1$1") + "\"";
		}

		public static string ActiveEntry(string root, string component)
		{
			string path = Path.Combine(root, ".launcher", component + ".json");
			if (!File.Exists(path))
			{
				return null;
			}
			ActiveRelease activeRelease = new JavaScriptSerializer().Deserialize<ActiveRelease>(File.ReadAllText(path));
			if (activeRelease == null || !Regex.IsMatch(activeRelease.directory ?? "", "^" + component + "-[0-9]+\\.[0-9]+\\.[0-9]+-[a-f0-9]{32}$"))
			{
				throw new InvalidDataException("Invalid installed release record.");
			}
			return Path.Combine(root, ".launcher", activeRelease.directory, (component == "launcher") ? "Burnout3Launcher.exe" : "Burnout3.exe");
		}

		[STAThread]
		public static int Main(string[] args)
		{
			try
			{
				string path = AppDomain.CurrentDomain.BaseDirectory;
				string text = null;
				string text2 = null;
				string text3 = null;
				string size = "864x580";
				int num;
				for (num = 0; num < args.Length; num++)
				{
					if (num + 1 >= args.Length)
					{
						throw new ArgumentException("Missing argument value.");
					}
					switch (args[num])
					{
					case "--root":
						path = args[++num];
						break;
					case "--preview":
						text = args[++num];
						break;
					case "--preview-size":
						size = args[++num];
						break;
					case "--self-check":
						text2 = args[++num];
						break;
					case "--check-feed":
						text3 = args[++num];
						break;
					default:
						throw new ArgumentException("Unknown launcher argument.");
					}
				}
				path = Path.GetFullPath(path);
				if (text3 != null)
				{
					Manifest manifest = Core.ReadManifest(Core.FetchText("/burn3/downloads/latest.json"));
					File.WriteAllText(text3, "launcher=" + ((manifest.launcher == null) ? "none" : manifest.launcher.version) + "\n" + LauncherWindow.FormatNews(Core.FetchText("/burn3/api/news")));
					return 0;
				}
				if (text2 != null)
				{
					File.WriteAllText(text2, new JavaScriptSerializer().Serialize(new
					{
						version = "0.0.13",
						root = path,
						executable = true,
						runtime = ActiveEntry(path, "runtime")
					}));
					return 0;
				}
				string text4 = ActiveEntry(path, "launcher");
				string location = Assembly.GetExecutingAssembly().Location;
				if (text == null && text4 != null && File.Exists(text4) && !Path.GetFullPath(text4).Equals(location, StringComparison.OrdinalIgnoreCase))
				{
					ProcessStartInfo processStartInfo = new ProcessStartInfo(text4, "--root " + Quote(path));
					processStartInfo.UseShellExecute = false;
					Process.Start(processStartInfo);
					return 0;
				}
				Application application = new Application();
				LauncherWindow launcherWindow = new LauncherWindow(path);
				if (text != null)
				{
					launcherWindow.Preview(text, size);
					return 0;
				}
				application.Run(launcherWindow.Window);
				return 0;
			}
			catch (Exception ex)
			{
				MessageBox.Show(ex.Message, "Burnout 3 launcher");
				return 1;
			}
		}
	}

	public sealed class ActiveRelease
	{
		public string version { get; set; }

		public string directory { get; set; }
	}

	public sealed class LauncherWindow
	{
		public sealed class NewsResponse
		{
			public ReleaseNews[] releases { get; set; }
		}

		public sealed class ReleaseNews
		{
			public string version { get; set; }

			public string text { get; set; }
		}

		private readonly string root;

		private readonly JavaScriptSerializer json = new JavaScriptSerializer();

		public readonly Window Window;

		private readonly TextBlock status;

		private readonly TextBlock news;

		private readonly TextBlock version;

		private readonly Button play;

		private readonly Button import;

		private readonly Button check;

		private readonly ProgressBar progress;

		private bool busy;

		private bool installing;

		private bool ready;

		private bool gameRunning;

		private bool modBusy;

		private string processedModSync;

		private readonly DispatcherTimer syncTimer = new DispatcherTimer();

		public static string FormatNews(string text)
		{
			NewsResponse newsResponse = new JavaScriptSerializer().Deserialize<NewsResponse>(text);
			if (newsResponse == null || newsResponse.releases == null)
			{
				throw new InvalidDataException("Invalid news response.");
			}
			return string.Join("\n\n", Array.ConvertAll(newsResponse.releases, (ReleaseNews r) => "v" + r.version + "\n" + r.text));
		}

		public LauncherWindow(string installRoot)
		{
			root = installRoot;
			using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("Window.xaml"))
			{
				Window = (Window)XamlReader.Load(stream);
			}
			status = (TextBlock)Window.FindName("Status");
			news = (TextBlock)Window.FindName("News");
			version = (TextBlock)Window.FindName("Version");
			play = (Button)Window.FindName("Play");
			import = (Button)Window.FindName("Import");
			check = (Button)Window.FindName("Check");
			progress = (ProgressBar)Window.FindName("Progress");
			ready = Core.GameReady(root);
			RefreshArtwork();
			version.Text = "Launcher v0.0.13  /  Game v" + InstalledVersion("runtime");
			string path = System.IO.Path.Combine(root, ".launcher", "news.txt");
			if (File.Exists(path))
			{
				news.Text = File.ReadAllText(path);
			}
			play.Click += async delegate
			{
				await CheckUpdates(true);
			};
			check.Click += async delegate
			{
				await CheckUpdates(false);
			};
			import.Click += async delegate
			{
				await Import();
			};
			((Button)Window.FindName("Account")).Click += delegate
			{
				Process.Start("https://uwucammie.com/account/");
			};
			((Button)Window.FindName("Mods")).Click += delegate
			{
				BrowseMods();
			};
			((Button)Window.FindName("Settings")).Click += delegate
			{
				ShowSettings();
			};
			((Button)Window.FindName("ImportSave")).Click += async delegate
			{
				await ImportSave();
			};
			syncTimer.Interval = TimeSpan.FromSeconds(1.0);
			syncTimer.Tick += async delegate
			{
				await CheckModSync();
			};
			syncTimer.Start();
			Window.Loaded += async delegate
			{
				await CheckUpdates(false);
			};
			Window.Closing += delegate(object sender, CancelEventArgs e)
			{
				if (installing || gameRunning || modBusy)
				{
					e.Cancel = true;
					status.Text = "Please wait for the current operation or game to finish.";
				}
			};
		}

		private string InstalledVersion(string component)
		{
			string path = System.IO.Path.Combine(root, ".launcher", component + ".json");
			if (File.Exists(path))
			{
				return json.Deserialize<ActiveRelease>(File.ReadAllText(path)).version;
			}
			if (component == "launcher")
			{
				return "0.0.13";
			}
			if (File.Exists(System.IO.Path.Combine(root, "build", "takedown-recomp.exe")) && File.Exists(System.IO.Path.Combine(root, "VERSION")))
			{
				return File.ReadAllText(System.IO.Path.Combine(root, "VERSION")).Trim();
			}
			return "0.0.0";
		}

		private void RefreshArtwork()
		{
			LauncherArtwork launcherArtwork = LauncherArtwork.Load(root);
			((Image)Window.FindName("DiscLogo")).Source = launcherArtwork.Logo;
			((Image)Window.FindName("DiscBackground")).Source = launcherArtwork.Background;
			((Rectangle)Window.FindName("DiscShade")).Visibility = ((launcherArtwork.Background == null) ? Visibility.Collapsed : Visibility.Visible);
			((Image)Window.FindName("DiscCar")).Source = launcherArtwork.Car;
			((StackPanel)Window.FindName("FallbackBrand")).Visibility = ((launcherArtwork.Logo != null) ? Visibility.Collapsed : Visibility.Visible);
		}

		private bool HasRuntime()
		{
			string text = Program.ActiveEntry(root, "runtime");
			if (text == null || !File.Exists(text))
			{
				return File.Exists(System.IO.Path.Combine(root, "run.ps1"));
			}
			return true;
		}

		private string ReadyMessage()
		{
			if (ready)
			{
				if (HasRuntime())
				{
					return "Ready to play.";
				}
				return "ISO ready. A downloadable game runtime has not been published yet.";
			}
			return "Choose your Burnout 3 ISO to set up the game.";
		}

		private void SetBusy(bool value, string message)
		{
			busy = value;
			status.Text = message;
			progress.Visibility = ((!value) ? Visibility.Hidden : Visibility.Visible);
			progress.IsIndeterminate = true;
			progress.Value = 0.0;
			check.IsEnabled = !value && !gameRunning;
			import.IsEnabled = !value && !ready && !gameRunning;
			play.IsEnabled = !value && ready && HasRuntime() && !gameRunning;
			if (gameRunning)
			{
				play.Content = "RUNNING";
			}
			else
			{
				play.Content = "PLAY";
			}
		}

		private void ReportUpdate(UpdateProgress update)
		{
			Window.Dispatcher.Invoke(delegate
			{
				progress.IsIndeterminate = update.Total <= 0;
				progress.Value = ((update.Total > 0) ? Math.Min(100.0, (double)update.Completed * 100.0 / (double)update.Total) : 0.0);
				string text = ((update.Component == "runtime") ? "game runtime" : ((update.Component == "launcher") ? "launcher" : update.Component));
				status.Text = update.Stage + " " + text;
				if (update.Total > 0 && update.Stage != "Complete")
				{
					status.Text += string.Format(": {0:0}% ({1:0.0} / {2:0.0} MB)", progress.Value, (double)update.Completed / 1000000.0, (double)update.Total / 1000000.0);
					if (update.BytesPerSecond > 0.0)
					{
						status.Text += string.Format("  {0:0.0} MB/s", update.BytesPerSecond / 1000000.0);
					}
				}
			});
		}

		private async Task Import()
		{
			if (busy)
			{
				return;
			}
			OpenFileDialog picker = new OpenFileDialog
			{
				Filter = "Burnout 3 disc image (*.iso)|*.iso",
				Title = "Choose your original US Burnout 3 ISO"
			};
			if (picker.ShowDialog(Window) != true)
			{
				return;
			}
			installing = true;
			SetBusy(true, "Setting up your game from the ISO...");
			try
			{
				await Task.Run(delegate
				{
					Core.ImportIso(picker.FileName, root);
				});
				ready = Core.GameReady(root);
				RefreshArtwork();
				await PrepareRuntime();
				RefreshArtwork();
				SetBusy(false, ReadyMessage());
			}
			catch (Exception ex)
			{
				SetBusy(false, ex.Message);
			}
			finally
			{
				installing = false;
			}
		}

		private static string RunSaveData(string executable, string root, string arguments, out int exitCode)
		{
			ProcessStartInfo processStartInfo = new ProcessStartInfo(executable, "save-data " + Program.Quote(System.IO.Path.Combine(root, "saves")) + " " + arguments);
			processStartInfo.UseShellExecute = false;
			processStartInfo.CreateNoWindow = true;
			processStartInfo.WorkingDirectory = root;
			processStartInfo.RedirectStandardOutput = true;
			processStartInfo.RedirectStandardError = true;
			ProcessStartInfo startInfo = processStartInfo;
			using (Process process = Process.Start(startInfo))
			{
				Task<string> task = process.StandardError.ReadToEndAsync();
				string text = process.StandardOutput.ReadToEnd();
				process.WaitForExit();
				exitCode = process.ExitCode;
				return (text + task.Result).Trim();
			}
		}

		private async Task ImportSave()
		{
			if (busy || gameRunning)
			{
				return;
			}
			string entry = Program.ActiveEntry(root, "runtime");
			if (!ready || entry == null)
			{
				status.Text = "Set up the game before importing save data.";
				return;
			}
			OpenFileDialog picker = new OpenFileDialog
			{
				Title = "Choose PCSX2 save data (a .ps2 file, or any file inside a folder card)",
				Filter = "PCSX2 memory card (*.ps2)|*.ps2|Any file inside a PCSX2 folder card or save folder|*.*"
			};
			if (picker.ShowDialog(Window) != true)
			{
				return;
			}
			string source = ((picker.FileName.EndsWith(".ps2", StringComparison.OrdinalIgnoreCase) && File.Exists(picker.FileName)) ? picker.FileName : System.IO.Path.GetDirectoryName(picker.FileName));
			SetBusy(true, "Reading save slots...");
			try
			{
				string executable = null;
				string slots = null;
				int code = 0;
				await Task.Run(delegate
				{
					executable = RuntimeSetup.Ensure(System.IO.Path.GetDirectoryName(entry), root, null);
					slots = RunSaveData(executable, root, "slots", out code);
				});
				if (code != 0)
				{
					throw new InvalidDataException(slots);
				}
				List<string> lines = new List<string>();
				string[] array = slots.Split('\n');
				foreach (string text in array)
				{
					string text2 = text.Trim().TrimStart('*').Trim();
					if (text2.StartsWith("Slot "))
					{
						lines.Add(text2);
					}
				}
				SetBusy(false, "Choose a slot for the imported save data.");
				int slot = ChooseSlot(lines);
				if (slot == 0)
				{
					SetBusy(false, ReadyMessage());
					return;
				}
				bool replace = !lines[slot - 1].EndsWith(": empty");
				SetBusy(true, "Importing save data...");
				string result = null;
				await Task.Run(delegate
				{
					result = RunSaveData(executable, root, "import " + Program.Quote(source) + " " + slot + (replace ? " --replace" : ""), out code);
				});
				SetBusy(false, (code == 0) ? ("Imported. " + result.Replace("\r", "").Replace("\n", "  ")) : ("Import failed: " + result));
			}
			catch (Exception ex)
			{
				SetBusy(false, "Import failed: " + ex.Message);
			}
		}

		private int ChooseSlot(List<string> slots)
		{
			Window dialog = new Window
			{
				Owner = Window,
				Title = "Import save data",
				Width = 520.0,
				SizeToContent = SizeToContent.Height,
				ResizeMode = ResizeMode.NoResize,
				WindowStartupLocation = WindowStartupLocation.CenterOwner,
				Background = new SolidColorBrush(Color.FromRgb(24, 28, 31)),
				Foreground = Brushes.White
			};
			StackPanel stackPanel = new StackPanel();
			stackPanel.Margin = new Thickness(24.0);
			StackPanel stackPanel2 = stackPanel;
			stackPanel2.Children.Add(new TextBlock
			{
				Text = "Choose a profile slot. The game uses the slot chosen in its SAVE DATA menu (Backspace in the main menu). Replacing a slot keeps its old saves in saves/backups.",
				TextWrapping = TextWrapping.Wrap,
				Margin = new Thickness(0.0, 0.0, 0.0, 16.0)
			});
			int chosen = 0;
			for (int i = 0; i < slots.Count; i++)
			{
				int slot = i + 1;
				bool flag = slots[i].EndsWith(": empty");
				Button button = new Button();
				button.Content = slots[i] + (flag ? "" : "  (replace)");
				button.Padding = new Thickness(12.0, 8.0, 12.0, 8.0);
				button.Margin = new Thickness(0.0, 0.0, 0.0, 8.0);
				button.HorizontalContentAlignment = HorizontalAlignment.Left;
				Button button2 = button;
				button2.Click += delegate
				{
					chosen = slot;
					dialog.DialogResult = true;
				};
				stackPanel2.Children.Add(button2);
			}
			Button button3 = new Button();
			button3.Content = "CANCEL";
			button3.Padding = new Thickness(12.0, 8.0, 12.0, 8.0);
			Button button4 = button3;
			button4.Click += delegate
			{
				dialog.DialogResult = false;
			};
			stackPanel2.Children.Add(button4);
			dialog.Content = stackPanel2;
			if (dialog.ShowDialog() != true)
			{
				return 0;
			}
			return chosen;
		}

		private async Task PrepareRuntime()
		{
			string entry = Program.ActiveEntry(root, "runtime");
			if (!ready || entry == null)
			{
				return;
			}
			SetBusy(true, "Checking installed game files...");
			await Task.Run(delegate
			{
				RuntimeSetup.Ensure(System.IO.Path.GetDirectoryName(entry), root, delegate(string message)
				{
					Window.Dispatcher.BeginInvoke((Action)delegate
					{
						status.Text = message;
					});
				});
			});
			await CleanUpOldReleases();
		}

		private async Task CleanUpOldReleases()
		{
			if (!gameRunning)
			{
				List<string> failures = new List<string>();
				string running = System.IO.Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
				long freed = await Task.Run(() => Core.PruneOldReleases(root, running, failures));
				string logs = System.IO.Path.Combine(root, ".launcher", "logs");
				Directory.CreateDirectory(logs);
				if (freed > 0 || failures.Count > 0)
				{
					File.AppendAllText(System.IO.Path.Combine(logs, "cleanup.log"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + string.Format(" freed {0:0.0} GB of old versions", (double)freed / 1073741824.0) + ((failures.Count > 0) ? ("; kept for now: " + string.Join(" | ", failures)) : "") + Environment.NewLine);
				}
			}
		}

		private async Task CheckUpdates(bool launch)
		{
			if (busy || gameRunning || modBusy)
			{
				return;
			}
			SetBusy(true, "Checking for updates...");
			try
			{
				Manifest manifest = null;
				string error = null;
				string newsText = null;
				await Task.Run(delegate
				{
					try
					{
						manifest = Core.ReadManifest(Core.FetchText("/burn3/downloads/latest.json"));
					}
					catch (Exception ex2)
					{
						error = ex2.Message;
					}
					try
					{
						newsText = Core.FetchText("/burn3/api/news");
					}
					catch
					{
					}
				});
				if (newsText != null)
				{
					news.Text = FormatNews(newsText);
					Core.AtomicText(System.IO.Path.Combine(root, ".launcher", "news.txt"), news.Text);
				}
				else if (news.Text == "Loading news...")
				{
					news.Text = "News is unavailable while offline.";
				}
				SetBusy(false, (error != null) ? "Could not check for updates. Installed games remain available." : ReadyMessage());
				if (manifest != null)
				{
					if (manifest.launcher != null && Core.ParseVersion(manifest.launcher.version) <= Core.ParseVersion(InstalledVersion("launcher")))
					{
						manifest.launcher = null;
					}
					if (manifest.runtime != null && Core.ParseVersion(manifest.runtime.version) <= Core.ParseVersion(InstalledVersion("runtime")))
					{
						manifest.runtime = null;
					}
					if ((manifest.launcher != null || manifest.runtime != null) && Offer(manifest, launch))
					{
						await Install(manifest);
						return;
					}
				}
				if (launch)
				{
					await Launch();
				}
			}
			catch (Exception ex)
			{
				SetBusy(false, ex.Message);
			}
		}

		private bool Offer(Manifest manifest, bool launch)
		{
			Window dialog = new Window
			{
				Owner = Window,
				Title = "Update available",
				Width = 460.0,
				SizeToContent = SizeToContent.Height,
				ResizeMode = ResizeMode.NoResize,
				WindowStartupLocation = WindowStartupLocation.CenterOwner,
				Background = new SolidColorBrush(Color.FromRgb(32, 36, 39)),
				Foreground = Brushes.White
			};
			StackPanel stackPanel = new StackPanel();
			stackPanel.Margin = new Thickness(24.0);
			StackPanel stackPanel2 = stackPanel;
			stackPanel2.Children.Add(new TextBlock
			{
				Text = "Available: " + ((manifest.launcher != null) ? ("launcher v" + manifest.launcher.version + "  ") : "") + ((manifest.runtime != null) ? ("game v" + manifest.runtime.version) : ""),
				TextWrapping = TextWrapping.Wrap,
				Margin = new Thickness(0.0, 0.0, 0.0, 18.0)
			});
			Button button = new Button();
			button.Content = "Update now";
			button.Padding = new Thickness(12.0);
			button.Background = Brushes.Orange;
			button.Margin = new Thickness(0.0, 0.0, 0.0, 10.0);
			Button button2 = button;
			Button button3 = new Button();
			button3.Content = (launch ? "Launch without updating" : "Not now");
			button3.Padding = new Thickness(12.0);
			Button button4 = button3;
			button2.Click += delegate
			{
				dialog.DialogResult = true;
			};
			button4.Click += delegate
			{
				dialog.DialogResult = false;
			};
			stackPanel2.Children.Add(button2);
			stackPanel2.Children.Add(button4);
			dialog.Content = stackPanel2;
			return dialog.ShowDialog() == true;
		}

		private async Task Install(Manifest manifest)
		{
			installing = true;
			SetBusy(true, "Preparing updates...");
			string runtimeRecord = System.IO.Path.Combine(root, ".launcher", "runtime.json");
			string previousRuntime = (File.Exists(runtimeRecord) ? File.ReadAllText(runtimeRecord) : null);
			try
			{
				await Task.Run(delegate
				{
					if (manifest.runtime != null)
					{
						Core.Install(root, "runtime", manifest.runtime, ReportUpdate);
					}
					if (manifest.launcher != null)
					{
						Core.Install(root, "launcher", manifest.launcher, ReportUpdate);
					}
				});
				await PrepareRuntime();
				string entry = Program.ActiveEntry(root, "launcher") ?? Assembly.GetExecutingAssembly().Location;
				Process.Start(new ProcessStartInfo(entry, "--root " + Program.Quote(root))
				{
					UseShellExecute = false
				});
				installing = false;
				Window.Close();
			}
			catch (Exception ex)
			{
				if (manifest.runtime != null && previousRuntime != null && File.Exists(runtimeRecord))
				{
					ActiveRelease activeRelease = json.Deserialize<ActiveRelease>(File.ReadAllText(runtimeRecord));
					if (activeRelease.version == manifest.runtime.version)
					{
						Core.AtomicText(runtimeRecord, previousRuntime);
					}
				}
				SetBusy(false, ex.Message);
			}
			finally
			{
				installing = false;
			}
		}

		private async Task Launch()
		{
			if (!ready || !HasRuntime())
			{
				SetBusy(false, ReadyMessage());
				return;
			}
			installing = true;
			try
			{
				await PrepareRuntime();
			}
			finally
			{
				installing = false;
			}
			string entry = Program.ActiveEntry(root, "runtime");
			ProcessStartInfo start = ((entry != null) ? new ProcessStartInfo(entry, "--install-root " + Program.Quote(root)) : new ProcessStartInfo("powershell.exe", "-NoProfile -ExecutionPolicy Bypass -File " + Program.Quote(System.IO.Path.Combine(root, "run.ps1"))));
			start.UseShellExecute = false;
			start.CreateNoWindow = true;
			start.WorkingDirectory = root;
			start.RedirectStandardOutput = true;
			start.RedirectStandardError = true;
			string logs = System.IO.Path.Combine(root, ".launcher", "logs");
			Directory.CreateDirectory(logs);
			string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss-fff");
			gameRunning = true;
			try
			{
				Process process = new Process
				{
					StartInfo = start
				};
				try
				{
					StreamWriter output = File.CreateText(System.IO.Path.Combine(logs, stamp + ".log"));
					try
					{
						StreamWriter errors = File.CreateText(System.IO.Path.Combine(logs, stamp + ".error.log"));
						try
						{
							process.OutputDataReceived += delegate(object s, DataReceivedEventArgs e)
							{
								if (e.Data != null)
								{
									output.WriteLine(e.Data);
								}
							};
							process.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e)
							{
								if (e.Data != null)
								{
									errors.WriteLine(e.Data);
								}
							};
							process.Start();
							process.BeginOutputReadLine();
							process.BeginErrorReadLine();
							SetBusy(false, "Game running. You can download mods here; use Refresh on the game's Mods screen to discover them.");
							await Task.Run(delegate
							{
								process.WaitForExit();
							});
							gameRunning = false;
							SetBusy(false, (process.ExitCode == 0) ? ReadyMessage() : "The game could not start. Details are in .launcher/logs.");
						}
						finally
						{
							if (errors != null)
							{
								((IDisposable)errors).Dispose();
							}
						}
					}
					finally
					{
						if (output != null)
						{
							((IDisposable)output).Dispose();
						}
					}
				}
				finally
				{
					if (process != null)
					{
						((IDisposable)process).Dispose();
					}
				}
			}
			finally
			{
				gameRunning = false;
				SetBusy(false, status.Text);
			}
		}

		private void ShowSettings()
		{
			LaunchSettings settings;
			try
			{
				settings = LaunchSettings.Load(root);
			}
			catch (Exception ex)
			{
				status.Text = ex.Message;
				return;
			}
			Window dialog = new Window
			{
				Owner = Window,
				Title = "Settings",
				Width = 560.0,
				SizeToContent = SizeToContent.Height,
				ResizeMode = ResizeMode.NoResize,
				WindowStartupLocation = WindowStartupLocation.CenterOwner,
				Background = new SolidColorBrush(Color.FromRgb(24, 28, 31)),
				Foreground = Brushes.White
			};
			StackPanel panel = new StackPanel
			{
				Margin = new Thickness(24.0),
				Background = dialog.Background
			};
			panel.Children.Add(new TextBlock
			{
				Text = "SETTINGS",
				FontSize = 22.0,
				FontWeight = FontWeights.Bold,
				Margin = new Thickness(0.0, 0.0, 0.0, 6.0)
			});
			panel.Children.Add(new TextBlock
			{
				Text = "Changes apply the next time you press Play.",
				TextWrapping = TextWrapping.Wrap,
				Foreground = new SolidColorBrush(Color.FromRgb(204, 208, 210)),
				Margin = new Thickness(0.0, 0.0, 0.0, 18.0)
			});
			panel.Children.Add(new TextBlock
			{
				Text = "Video output",
				FontWeight = FontWeights.Bold,
				Margin = new Thickness(0.0, 0.0, 0.0, 6.0)
			});
			RadioButton radioButton = new RadioButton();
			radioButton.GroupName = "scan";
			radioButton.IsChecked = !settings.Interlaced;
			radioButton.Foreground = Brushes.White;
			radioButton.Margin = new Thickness(0.0, 0.0, 0.0, 4.0);
			radioButton.Content = new TextBlock
			{
				Text = "Progressive (480p)"
			};
			RadioButton element = radioButton;
			RadioButton interlaced = new RadioButton
			{
				GroupName = "scan",
				IsChecked = settings.Interlaced,
				Foreground = Brushes.White,
				Margin = new Thickness(0.0, 0.0, 0.0, 4.0),
				Content = new TextBlock
				{
					Text = "Interlaced (480i)"
				}
			};
			panel.Children.Add(element);
			panel.Children.Add(interlaced);
			panel.Children.Add(new TextBlock
			{
				Text = "Progressive is the sharpest picture on modern screens. Interlaced is the game's standard TV mode, for CRT TVs and monitors running a 480i display mode.",
				TextWrapping = TextWrapping.Wrap,
				Foreground = new SolidColorBrush(Color.FromRgb(157, 164, 168)),
				Margin = new Thickness(22.0, 0.0, 0.0, 16.0)
			});
			panel.Children.Add(new TextBlock
			{
				Text = "Troubleshooting",
				FontWeight = FontWeights.Bold,
				Margin = new Thickness(0.0, 0.0, 0.0, 6.0)
			});
			Func<string, string, bool, CheckBox> func = delegate(string title, string detail, bool value)
			{
				CheckBox checkBox = new CheckBox
				{
					IsChecked = value,
					Foreground = Brushes.White,
					Margin = new Thickness(0.0, 0.0, 0.0, 4.0),
					Content = new TextBlock
					{
						Text = title,
						FontWeight = FontWeights.Bold
					}
				};
				panel.Children.Add(checkBox);
				panel.Children.Add(new TextBlock
				{
					Text = detail,
					TextWrapping = TextWrapping.Wrap,
					Foreground = new SolidColorBrush(Color.FromRgb(157, 164, 168)),
					Margin = new Thickness(22.0, 0.0, 0.0, 16.0)
				});
				return checkBox;
			};
			CheckBox reports = func("Save GPU crash reports", "If the graphics card crashes, the game writes a report into .launcher/logs that shows what it was drawing. Slightly slower, so leave it off unless you are sending in a crash.", settings.gpu_crash_reports);
			CheckBox descriptors = func("Use descriptor buffers", "A newer way of handing textures to the graphics card. Turn this off if the game crashes after the first frames on older cards, for example an RX 580.", settings.descriptor_buffers);
			StackPanel stackPanel = new StackPanel();
			stackPanel.Orientation = Orientation.Horizontal;
			stackPanel.Margin = new Thickness(0.0, 8.0, 0.0, 0.0);
			StackPanel stackPanel2 = stackPanel;
			Button button = new Button();
			button.Content = "SAVE";
			button.Padding = new Thickness(16.0, 10.0, 16.0, 10.0);
			button.Margin = new Thickness(0.0, 0.0, 10.0, 0.0);
			button.Background = new SolidColorBrush(Color.FromRgb(233, 150, 32));
			button.Foreground = new SolidColorBrush(Color.FromRgb(16, 18, 20));
			Button button2 = button;
			Button button3 = new Button();
			button3.Content = "CANCEL";
			button3.Padding = new Thickness(16.0, 10.0, 16.0, 10.0);
			button3.Margin = new Thickness(0.0, 0.0, 10.0, 0.0);
			Button button4 = button3;
			Button button5 = new Button();
			button5.Content = "OPEN LOGS FOLDER";
			button5.Padding = new Thickness(16.0, 10.0, 16.0, 10.0);
			Button button6 = button5;
			stackPanel2.Children.Add(button2);
			stackPanel2.Children.Add(button4);
			stackPanel2.Children.Add(button6);
			panel.Children.Add(stackPanel2);
			TextBlock error = new TextBlock
			{
				TextWrapping = TextWrapping.Wrap,
				Foreground = new SolidColorBrush(Color.FromRgb(byte.MaxValue, 170, 47)),
				Margin = new Thickness(0.0, 12.0, 0.0, 0.0)
			};
			panel.Children.Add(error);
			dialog.Content = panel;
			button2.Click += delegate
			{
				settings.gpu_crash_reports = reports.IsChecked == true;
				settings.descriptor_buffers = descriptors.IsChecked == true;
				settings.scan_mode = ((interlaced.IsChecked == true) ? "interlaced" : "progressive");
				try
				{
					settings.Save(root);
					dialog.DialogResult = true;
				}
				catch (Exception ex2)
				{
					error.Text = "Could not save settings: " + ex2.Message;
				}
			};
			button4.Click += delegate
			{
				dialog.DialogResult = false;
			};
			button6.Click += delegate
			{
				string text = System.IO.Path.Combine(root, ".launcher", "logs");
				Directory.CreateDirectory(text);
				Process.Start(new ProcessStartInfo("explorer.exe", Program.Quote(text)));
			};
			if (dialog.ShowDialog() == true)
			{
				status.Text = "Settings saved. They apply the next time you press Play.";
			}
		}

		private void BrowseMods()
		{
			Window dialog = new Window
			{
				Owner = Window,
				Title = "Mod browser",
				Width = 720.0,
				Height = 620.0,
				MinWidth = 540.0,
				MinHeight = 450.0,
				WindowStartupLocation = WindowStartupLocation.CenterOwner,
				Background = new SolidColorBrush(Color.FromRgb(24, 28, 31)),
				Foreground = Brushes.White
			};
			Grid grid = new Grid();
			grid.Margin = new Thickness(22.0);
			Grid grid2 = grid;
			grid2.RowDefinitions.Add(new RowDefinition
			{
				Height = GridLength.Auto
			});
			grid2.RowDefinitions.Add(new RowDefinition
			{
				Height = new GridLength(1.0, GridUnitType.Star)
			});
			grid2.RowDefinitions.Add(new RowDefinition
			{
				Height = new GridLength(145.0)
			});
			grid2.RowDefinitions.Add(new RowDefinition
			{
				Height = GridLength.Auto
			});
			StackPanel stackPanel = new StackPanel();
			stackPanel.Orientation = Orientation.Horizontal;
			stackPanel.Margin = new Thickness(0.0, 0.0, 0.0, 16.0);
			StackPanel stackPanel2 = stackPanel;
			Button communityTab = new Button
			{
				Content = "COMMUNITY MODS",
				FontSize = 18.0,
				FontWeight = FontWeights.Bold,
				Padding = new Thickness(12.0, 6.0, 12.0, 6.0),
				Margin = new Thickness(0.0, 0.0, 10.0, 0.0)
			};
			Button installedTab = new Button
			{
				Content = "INSTALLED MODS",
				FontSize = 18.0,
				FontWeight = FontWeights.Bold,
				Padding = new Thickness(12.0, 6.0, 12.0, 6.0)
			};
			stackPanel2.Children.Add(communityTab);
			stackPanel2.Children.Add(installedTab);
			ListBox list = new ListBox
			{
				Background = new SolidColorBrush(Color.FromRgb(34, 39, 43)),
				Foreground = Brushes.White,
				Padding = new Thickness(10.0)
			};
			TextBlock details = new TextBlock
			{
				Text = "Loading the mod catalog...",
				TextWrapping = TextWrapping.Wrap,
				Margin = new Thickness(0.0, 14.0, 0.0, 12.0)
			};
			ScrollViewer scrollViewer = new ScrollViewer();
			scrollViewer.Content = details;
			scrollViewer.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
			ScrollViewer element = scrollViewer;
			StackPanel stackPanel3 = new StackPanel();
			stackPanel3.Orientation = Orientation.Horizontal;
			StackPanel stackPanel4 = stackPanel3;
			Button download = new Button
			{
				Content = "DOWNLOAD",
				Padding = new Thickness(16.0, 10.0, 16.0, 10.0),
				IsEnabled = false
			};
			Button reload = new Button
			{
				Content = "RELOAD",
				Padding = new Thickness(16.0, 10.0, 16.0, 10.0),
				Margin = new Thickness(10.0, 0.0, 0.0, 0.0)
			};
			Button remove = new Button
			{
				Content = "REMOVE",
				Padding = new Thickness(16.0, 10.0, 16.0, 10.0),
				Margin = new Thickness(10.0, 0.0, 0.0, 0.0),
				IsEnabled = false
			};
			stackPanel4.Children.Add(download);
			stackPanel4.Children.Add(reload);
			stackPanel4.Children.Add(remove);
			Grid.SetRow(list, 1);
			Grid.SetRow(element, 2);
			Grid.SetRow(stackPanel4, 3);
			grid2.Children.Add(stackPanel2);
			grid2.Children.Add(list);
			grid2.Children.Add(element);
			grid2.Children.Add(stackPanel4);
			dialog.Content = grid2;
			bool showingInstalled = false;
			CatalogMod[] catalog = null;
			SolidColorBrush tabColour = new SolidColorBrush(Color.FromRgb(233, 150, 32));
			SolidColorBrush activeText = new SolidColorBrush(Color.FromRgb(16, 18, 20));
			Func<CatalogMod, LocalMod> localOf = delegate(CatalogMod mod)
			{
				foreach (LocalMod item in Core.ListLocalMods(root))
				{
					if (item.CatalogId == mod.id)
					{
						return item;
					}
				}
				return (LocalMod)null;
			};
			Action selection = delegate
			{
				remove.IsEnabled = false;
				if (showingInstalled)
				{
					LocalMod localMod = list.SelectedItem as LocalMod;
					download.IsEnabled = false;
					if (localMod != null)
					{
						remove.IsEnabled = !modBusy && !gameRunning;
						details.Text = localMod.Title + (string.IsNullOrEmpty(localMod.Version) ? "" : ("  v" + localMod.Version)) + "\n\nFolders: " + string.Join(", ", localMod.Folders) + ((localMod.CatalogId != null) ? "\nDownloaded from the community catalog." : "\nAdded by hand.") + (gameRunning ? "\nClose the game before removing a mod." : "");
					}
				}
				else
				{
					CatalogMod catalogMod = list.SelectedItem as CatalogMod;
					download.IsEnabled = catalogMod != null && !modBusy;
					if (catalogMod != null)
					{
						string text = Core.InstalledModVersion(root, catalogMod);
						remove.IsEnabled = text != null && !modBusy && !gameRunning;
						download.Content = ((text == catalogMod.version) ? "VERIFY / REINSTALL" : ((text == null) ? "DOWNLOAD" : "UPDATE MOD"));
						details.Text = catalogMod.description + "\n\n" + ((text == null) ? "Not installed" : ("Installed v" + text)) + string.Format("  •  {0:0.0} MB", (double)catalogMod.bytes / 1000000.0) + "\nUse Refresh in the game's Mods screen after downloading, then enable the mod.";
					}
				}
			};
			Action showLocal = delegate
			{
				try
				{
					list.ItemsSource = Core.ListLocalMods(root);
					details.Text = ((list.Items.Count == 0) ? "No mods are installed." : "Choose a mod to view it or remove it.");
				}
				catch (Exception ex)
				{
					list.ItemsSource = null;
					details.Text = "Could not read the mods folder: " + ex.Message;
				}
			};
			Func<Task> load = async delegate
			{
				reload.IsEnabled = false;
				download.IsEnabled = false;
				remove.IsEnabled = false;
				try
				{
					int num = default(int);
					int num2 = num;
					int num3 = 0;
					try
					{
						if (showingInstalled)
						{
							showLocal();
						}
						else
						{
							catalog = Core.ReadModCatalog(await Task.Run(() => Core.FetchText("/burn3/api/mods"))).mods;
							if (!showingInstalled)
							{
								list.ItemsSource = catalog;
								details.Text = ((list.Items.Count == 0) ? "No community mods have been published yet." : "Choose a mod to view details and download it.");
							}
						}
					}
					catch (Exception ex)
					{
						details.Text = "Could not load mods: " + ex.Message + "\nInstalled mods remain available.";
					}
				}
				finally
				{
					reload.IsEnabled = true;
				}
			};
			Action<bool> showTab = delegate(bool installedView)
			{
				showingInstalled = installedView;
				communityTab.ClearValue(Control.BackgroundProperty);
				installedTab.ClearValue(Control.BackgroundProperty);
				communityTab.ClearValue(Control.ForegroundProperty);
				installedTab.ClearValue(Control.ForegroundProperty);
				if (installedView)
				{
					installedTab.Background = tabColour;
					installedTab.Foreground = activeText;
				}
				else
				{
					communityTab.Background = tabColour;
					communityTab.Foreground = activeText;
				}
				download.Visibility = (installedView ? Visibility.Collapsed : Visibility.Visible);
				reload.Margin = new Thickness((!installedView) ? 10 : 0, 0.0, 0.0, 0.0);
				if (installedView)
				{
					showLocal();
				}
				else if (catalog != null)
				{
					list.ItemsSource = catalog;
					details.Text = "Choose a mod to view details and download it.";
				}
				else
				{
					list.ItemsSource = null;
					details.Text = "Loading the mod catalog...";
				}
				remove.IsEnabled = false;
			};
			communityTab.Click += async delegate
			{
				showTab(false);
				if (catalog == null)
				{
					await load();
				}
			};
			installedTab.Click += delegate
			{
				showTab(true);
			};
			list.SelectionChanged += delegate
			{
				try
				{
					selection();
				}
				catch (Exception ex)
				{
					details.Text = ex.Message;
				}
			};
			reload.Click += async delegate
			{
				await load();
			};
			remove.Click += delegate
			{
				LocalMod localMod = (showingInstalled ? (list.SelectedItem as LocalMod) : null);
				CatalogMod catalogMod = (showingInstalled ? null : (list.SelectedItem as CatalogMod));
				try
				{
					if (catalogMod != null)
					{
						localMod = localOf(catalogMod);
					}
				}
				catch (Exception ex)
				{
					details.Text = ex.Message;
					return;
				}
				if (localMod != null && !modBusy && !installing && !busy)
				{
					if (gameRunning)
					{
						details.Text = "Close the game before removing a mod.";
					}
					else if (MessageBox.Show(dialog, "Remove " + localMod.Title + "? Its files are deleted from the mods folder. You can download it again later if it is in the community catalog.", "Remove mod", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
					{
						modBusy = true;
						ListBox listBox = list;
						Button button = reload;
						Button button2 = download;
						bool flag = (remove.IsEnabled = false);
						bool flag3 = (button2.IsEnabled = flag);
						bool isEnabled = (button.IsEnabled = flag3);
						listBox.IsEnabled = isEnabled;
						try
						{
							Core.RemoveLocalMod(root, localMod);
							if (showingInstalled)
							{
								showLocal();
							}
							else
							{
								selection();
							}
							details.Text = "Removed " + localMod.Title + ".";
						}
						catch (Exception ex2)
						{
							details.Text = "Could not remove the mod: " + ex2.Message;
						}
						finally
						{
							modBusy = false;
							ListBox listBox2 = list;
							bool isEnabled2 = (reload.IsEnabled = true);
							listBox2.IsEnabled = isEnabled2;
						}
					}
				}
			};
			download.Click += async delegate
			{
				CatalogMod mod = list.SelectedItem as CatalogMod;
				if (mod == null || modBusy || installing || busy)
				{
					return;
				}
				modBusy = true;
				ListBox listBox = list;
				Button button = reload;
				bool flag = (download.IsEnabled = false);
				bool isEnabled = (button.IsEnabled = flag);
				listBox.IsEnabled = isEnabled;
				try
				{
					int num = default(int);
					int num4 = num;
					int num5 = 0;
					try
					{
						await Task.Run(delegate
						{
							Core.InstallMod(root, mod, delegate(UpdateProgress update)
							{
								dialog.Dispatcher.Invoke(delegate
								{
									details.Text = update.Stage + " " + mod.title + ((update.Total > 0) ? string.Format("  {0:0}%", (double)update.Completed * 100.0 / (double)update.Total) : "");
								});
							});
							string text = Program.ActiveEntry(root, "runtime");
							if (text != null)
							{
								try
								{
									RuntimeSetup.PrepareCarThumbnails(System.IO.Path.GetDirectoryName(text), root, null, null);
								}
								catch
								{
								}
							}
						});
						details.Text = "Installed " + mod.title + ". Use Refresh on the game's Mods screen, then enable it when you are ready.";
					}
					catch (Exception ex)
					{
						details.Text = "Could not install mod: " + ex.Message;
					}
				}
				finally
				{
					modBusy = false;
					ListBox listBox2 = list;
					Button button2 = reload;
					bool flag4 = (download.IsEnabled = true);
					bool isEnabled2 = (button2.IsEnabled = flag4);
					listBox2.IsEnabled = isEnabled2;
					remove.IsEnabled = Core.InstalledModVersion(root, mod) != null && !gameRunning;
				}
			};
			dialog.Closing += delegate(object sender, CancelEventArgs e)
			{
				if (modBusy)
				{
					e.Cancel = true;
				}
			};
			dialog.Loaded += async delegate
			{
				showTab(false);
				await load();
			};
			dialog.Show();
		}

		private async Task CheckModSync()
		{
			if (modBusy || installing || busy)
			{
				return;
			}
			string path = System.IO.Path.Combine(root, ".launcher", "mod-sync", "request.json");
			if (!File.Exists(path))
			{
				return;
			}
			ModSyncRequest request;
			try
			{
				request = Core.ReadModSyncRequest(path);
			}
			catch
			{
				return;
			}
			if (request.request_id == processedModSync)
			{
				return;
			}
			string result = System.IO.Path.Combine(root, ".launcher", "mod-sync", "result.json");
			if (File.Exists(result))
			{
				try
				{
					Dictionary<string, object> dictionary = json.Deserialize<Dictionary<string, object>>(File.ReadAllText(result));
					if ((string)dictionary["request_id"] == request.request_id)
					{
						processedModSync = request.request_id;
						return;
					}
				}
				catch
				{
				}
			}
			processedModSync = request.request_id;
			modBusy = true;
			try
			{
				int num = default(int);
				int num2 = num;
				int num3 = 0;
				try
				{
					progress.Visibility = Visibility.Visible;
					await Task.Run(delegate
					{
						Core.SynchronizeMods(root, request, ReportUpdate);
						string text = Program.ActiveEntry(root, "runtime");
						if (text != null)
						{
							try
							{
								RuntimeSetup.PrepareCarThumbnails(System.IO.Path.GetDirectoryName(text), root, null, null);
							}
							catch
							{
							}
						}
					});
					status.Text = "Host mods downloaded. Select Refresh on the game's Mods screen to apply them and join the lobby.";
				}
				catch (Exception ex)
				{
					status.Text = "Host mod download failed: " + ex.Message + " Retry joining to try again.";
				}
			}
			finally
			{
				modBusy = false;
				progress.Visibility = Visibility.Hidden;
			}
		}

		public void Preview(string path, string size)
		{
			news.Text = "v0.0.1\nInitial burn3 release";
			SetBusy(false, ReadyMessage());
			string[] array = size.Split('x');
			int result;
			int result2;
			if (array.Length != 2 || !int.TryParse(array[0], out result) || !int.TryParse(array[1], out result2) || result < 1 || result2 < 1)
			{
				throw new ArgumentException("--preview-size wants WIDTHxHEIGHT.");
			}
			FrameworkElement frameworkElement = (FrameworkElement)Window.Content;
			frameworkElement.Measure(new Size(result, result2));
			frameworkElement.Arrange(new Rect(0.0, 0.0, result, result2));
			frameworkElement.UpdateLayout();
			RenderTargetBitmap renderTargetBitmap = new RenderTargetBitmap(result, result2, 96.0, 96.0, PixelFormats.Pbgra32);
			renderTargetBitmap.Render(frameworkElement);
			PngBitmapEncoder pngBitmapEncoder = new PngBitmapEncoder();
			pngBitmapEncoder.Frames.Add(BitmapFrame.Create(renderTargetBitmap));
			using (FileStream stream = File.Create(path))
			{
				pngBitmapEncoder.Save(stream);
			}
		}
	}

	public sealed class LauncherArtwork
	{
		public BitmapSource Logo { get; private set; }

		public BitmapSource Background { get; private set; }

		public BitmapSource Car { get; private set; }

		public static LauncherArtwork Load(string root)
		{
			LauncherArtwork launcherArtwork = new LauncherArtwork();
			launcherArtwork.Logo = Cached(root, "titlescreen_logo.png") ?? Disc(root, "FRONTEND.TXD", "titlescreen_logo") ?? Cached(root, "B3Logo.png") ?? Disc(root, "GLOBAL.TXD", "B3Logo");
			launcherArtwork.Background = Cached(root, "bg.png") ?? Disc(root, "GLOBAL.TXD", "bg");
			launcherArtwork.Car = Cached(root, "COMPCAR1.png") ?? Disc(root, "FRONTEND.TXD", "COMPCAR1");
			return launcherArtwork;
		}

		private static BitmapSource Cached(string root, string name)
		{
			try
			{
				string text = Path.Combine(root, "cache", "ui", name);
				if (!File.Exists(text) || new FileInfo(text).Length > 16777216)
				{
					return null;
				}
				using (FileStream bitmapStream = File.OpenRead(text))
				{
					BitmapDecoder bitmapDecoder = BitmapDecoder.Create(bitmapStream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
					if (bitmapDecoder.Frames.Count != 1 || bitmapDecoder.Frames[0].PixelWidth > 2048 || bitmapDecoder.Frames[0].PixelHeight > 2048)
					{
						return null;
					}
					BitmapFrame bitmapFrame = bitmapDecoder.Frames[0];
					bitmapFrame.Freeze();
					return bitmapFrame;
				}
			}
			catch (Exception)
			{
				return null;
			}
		}

		private static BitmapSource Disc(string root, string file, string wanted)
		{
			try
			{
				string text = Path.Combine(root, "game", "DATA", file);
				if (!File.Exists(text) || new FileInfo(text).Length > 67108864)
				{
					return null;
				}
				byte[] array = File.ReadAllBytes(text);
				if (array.Length < 24 || U64(array, 0) != 13609576263680000000uL)
				{
					return null;
				}
				uint num = BitConverter.ToUInt32(array, 8);
				if (num > 4096 || 24 + (long)num * 16L > array.Length)
				{
					return null;
				}
				for (int i = 0; i < num; i++)
				{
					uint num2 = BitConverter.ToUInt32(array, 24 + i * 16);
					if (num2 <= array.Length - 200)
					{
						int num3 = (int)num2;
						int j;
						for (j = num3 + 168; j < num3 + 200 && array[j] != 0; j++)
						{
						}
						if (Encoding.ASCII.GetString(array, num3 + 168, j - num3 - 168) == wanted)
						{
							return Decode(array, num3);
						}
					}
				}
			}
			catch (Exception)
			{
			}
			return null;
		}

		private static ulong U64(byte[] data, int at)
		{
			return BitConverter.ToUInt64(data, at);
		}

		private static int Block(int x, int y)
		{
			return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2);
		}

		private static int Address32(int x, int y, int bw)
		{
			int num = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((x & 4) << 1);
			return ((y >> 5) * bw + (x >> 6)) * 8192 + Block((x & 0x3F) >> 3, (y & 0x1F) >> 3) * 256 + ((y & 7) >> 1) * 64 + num * 4;
		}

		private static int Address16(int x, int y, int bw)
		{
			int num = (x & 0x3F) >> 4;
			int num2 = (y & 0x3F) >> 3;
			int num3 = (num2 & 1) | ((num & 1) << 1) | ((num2 & 2) << 1) | ((num & 2) << 2) | ((num2 & 4) << 2);
			int num4 = ((x & 8) >> 3) | ((x & 1) << 1) | ((y & 1) << 2) | ((x & 2) << 2) | ((x & 4) << 2);
			return ((y >> 6) * bw + (x >> 6)) * 8192 + num3 * 256 + ((y & 7) >> 1) * 64 + num4 * 2;
		}

		private static int Address8(int x, int y, int bw)
		{
			int num = (y & 0xF) >> 2;
			int num2 = ((y & 2) >> 1) | ((x & 8) >> 2) | ((x & 1) << 2) | ((y & 1) << 3) | ((x & 2) << 3) | (((x & 4) << 3) ^ ((y & 2) << 4) ^ ((num & 1) << 5));
			return ((y >> 6) * (bw >> 1) + (x >> 7)) * 8192 + Block((x & 0x7F) >> 4, (y & 0x3F) >> 4) * 256 + num * 64 + num2;
		}

		private static int Address4(int x, int y, int bw)
		{
			int num = (x & 0x7F) >> 5;
			int num2 = (y & 0x7F) >> 4;
			int num3 = (y & 0xF) >> 2;
			int num4 = (num2 & 1) | ((num & 1) << 1) | ((num2 & 2) << 1) | ((num & 2) << 2) | ((num2 & 4) << 2);
			int num5 = ((y & 2) >> 1) | ((x & 8) >> 2) | ((x & 0x10) >> 2) | ((x & 1) << 3) | ((y & 1) << 4) | ((x & 2) << 4) | (((x & 4) << 4) ^ ((y & 2) << 5) ^ ((num3 & 1) << 6));
			return (((y >> 7) * (bw >> 1) + (x >> 7)) * 8192 + num4 * 256 + num3 * 64) * 2 + num5;
		}

		private static BitmapSource Decode(byte[] data, int at)
		{
			ulong num = U64(data, at + 60);
			ulong num2 = U64(data, at + 304);
			ulong num3 = U64(data, at + 544);
			int num4 = (int)(num >> 20) & 0x3F;
			int num5 = (int)(num >> 14) & 0x3F;
			int num6 = 1 << ((int)(num >> 26) & 0xF);
			int num7 = 1 << ((int)(num >> 30) & 0xF);
			int num8 = (int)num3 & 0xFFF;
			int num9 = (int)(num3 >> 32) & 0xFFF;
			int num10 = ((int)U64(data, at + 576) & 0x7FFF) * 16;
			int num11 = (int)(num2 >> 48) & 0x3F;
			int num12 = (int)(num2 >> 56) & 0x3F;
			int num13 = ((num12 == 0) ? 4 : 2);
			if ((num4 != 19 && num4 != 20) || (num12 != 0 && num12 != 2) || num5 == 0 || num11 == 0 || num6 > 1024 || num7 > 1024 || num8 == 0 || num9 == 0 || num8 > 2048 || num9 > 2048 || (long)num8 * (long)num9 * num13 > num10 || (long)at + 592L + num10 > data.Length)
			{
				throw new InvalidDataException("Unsupported menu pixels.");
			}
			byte[] array = new byte[4194304];
			for (int i = 0; i < num9; i++)
			{
				for (int j = 0; j < num8; j++)
				{
					int dstOffset = ((num12 == 0) ? Address32(j, i, num11) : Address16(j, i, num11));
					Buffer.BlockCopy(data, at + 592 + (i * num8 + j) * num13, array, dstOffset, num13);
				}
			}
			int num14 = -1;
			int num15 = at + 592 + num10;
			for (int k = num15; k + 80 <= Math.Min(data.Length, num15 + 128); k++)
			{
				if (U64(data, k) == 1152921504606846979L && U64(data, k + 8) == 14)
				{
					num14 = k;
					break;
				}
			}
			if (num14 < 0)
			{
				throw new InvalidDataException("Missing menu palette.");
			}
			ulong num16 = U64(data, num14 + 16);
			ulong num17 = U64(data, num14 + 32);
			int num18 = (int)num17 & 0xFFF;
			int num19 = (int)(num17 >> 32) & 0xFFF;
			int num20 = (int)(num16 >> 32) & 0x7FF;
			int num21 = (int)(num16 >> 48) & 0x7FF;
			int num22 = (int)(U64(data, at + 368) >> 48) & 0x3F;
			if (num18 == 0 || num19 == 0 || num18 > 256 || num19 > 256 || num22 == 0 || (long)num14 + 80L + (long)num18 * (long)num19 * 4 > data.Length)
			{
				throw new InvalidDataException("Invalid menu palette.");
			}
			byte[] array2 = new byte[4194304];
			for (int l = 0; l < num19; l++)
			{
				for (int m = 0; m < num18; m++)
				{
					Buffer.BlockCopy(data, num14 + 80 + (l * num18 + m) * 4, array2, Address32(m + num20, l + num21, num22), 4);
				}
			}
			byte[] array3 = new byte[((num4 == 19) ? 256 : 16) * 4];
			for (int n = 0; n < array3.Length / 4; n++)
			{
				int num23 = (n & 0xE7) | ((n & 8) << 1) | ((n & 0x10) >> 1);
				Buffer.BlockCopy(array2, Address32((num23 & 0xF) + num20, (num23 >> 4) + num21, num22), array3, n * 4, 4);
			}
			byte[] array4 = new byte[num6 * num7 * 4];
			for (int num24 = 0; num24 < num7; num24++)
			{
				for (int num25 = 0; num25 < num6; num25++)
				{
					int num26 = ((num4 == 19) ? Address8(num25, num24, num5) : Address4(num25, num24, num5));
					int num27 = ((num4 == 19) ? array[num26] : ((array[num26 >> 1] >> (num26 & 1) * 4) & 0xF));
					int num28 = (num24 * num6 + num25) * 4;
					int num29 = num27 * 4;
					array4[num28] = array3[num29 + 2];
					array4[num28 + 1] = array3[num29 + 1];
					array4[num28 + 2] = array3[num29];
					array4[num28 + 3] = (byte)Math.Min(255, array3[num29 + 3] * 2);
				}
			}
			BitmapSource bitmapSource = BitmapSource.Create(num6, num7, 96.0, 96.0, PixelFormats.Bgra32, null, array4, num6 * 4);
			bitmapSource.Freeze();
			return bitmapSource;
		}
	}
}
