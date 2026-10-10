using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace NvofControl
{
    public sealed class SettingsStore
    {
        private readonly string path;
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
        private static extern uint GetPrivateProfileInt(string section, string key, int fallback, string file);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WritePrivateProfileString(string section, string key, string value, string file);
        public SettingsStore(string path) { this.path = path; }
        public bool Enabled { get { return GetPrivateProfileInt("Nvof", "Enabled", 1, path) != 0; } }
        public void MigrateToDoubleRate()
        {
            // Keep DoubleRate=1 for rollback compatibility; fixed targets are obsolete.
            Write("DoubleRate", "1");
            Write("TargetFps", null);
        }
        public int InputRateMask { get { return (int)(GetPrivateProfileInt("Nvof", "InputRateMask", 63, path) & 63); } }
        public void SetInputRateMask(int value)
        {
            if (value < 0 || value > 63) throw new ArgumentOutOfRangeException("value");
            Write("InputRateMask", value.ToString(CultureInfo.InvariantCulture));
        }
        public void SetEnabled(bool value) { Write("Enabled", value ? "1" : "0"); }
        public bool NativeSynthesis { get { return true; } }
        public bool GpuCorrection { get { return GetPrivateProfileInt("Nvof", "GpuMidpointCorrection", 1, path) != 0; } }
        public bool AppearanceProtection { get { return GetPrivateProfileInt("Nvof", "AppearanceProtection", 1, path) != 0; } }
        public void SetGpuCorrection(bool value) { Write("GpuMidpointCorrection", value ? "1" : "0"); }
        public void SetAppearanceProtection(bool value) { Write("AppearanceProtection", value ? "1" : "0"); }
        private void Write(string key, string value)
        {
            if (!WritePrivateProfileString("Nvof", key, value, path))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
    }

    public sealed class RuntimeStatus
    {
        public int ProcessId;
        public string State = "waiting";
        public double InputFps;
        public double OutputFps;
        public long OutputFrames;
        public string Message = "";
        public string Transport = "";
        public string BypassReason = "";
        public bool InputRateSelected = true;
        public int InputRateMask = 63;
        public bool DoubleRate;
        public bool? GpuCorrection, AppearanceProtection;
        public long MidpointPassFrames, AppearancePassFrames;
        public string TransportLabel
        {
            get
            {
                if (Transport == "d3d11-gpu") return "GPU 메모리로 직접 전달";
                if (Transport == "native-pending") return "GPU 전달 확인 중";
                if (Transport == "system-memory") return "RAM(CPU 메모리) 경유";
                return String.IsNullOrEmpty(Transport) ? "아직 보고 없음" : "확인되지 않은 전달 방식 (" + Transport + ")";
            }
        }
        public string TransportDetail
        {
            get
            {
                if (Transport == "d3d11-gpu") return "필터가 Direct3D 11 GPU 메모리 전달 경로를 보고했습니다.";
                if (Transport == "native-pending") return "GPU 연결은 준비됐지만 실제 영상 프레임의 전달은 아직 확인되지 않았습니다.";
                if (Transport == "system-memory") return "현재 영상은 RAM(CPU 메모리)을 경유합니다. 내장 디코더·렌더러의 네이티브 GPU 연결은 아직 확인되지 않았습니다.";
                return String.IsNullOrEmpty(Transport) ? "영상을 재생하면 실제 영상 전달 경로가 표시됩니다." : "보고된 전달 방식이 이 설정창에서 지원하는 값과 다릅니다. 진단 정보를 확인해 주세요.";
            }
        }
        public DateTime UpdatedUtc;
        public bool Exists;
        public bool ProcessAlive;
        public string ReadError = "";
        private static T Value<T>(JsonElement data, string name, T fallback)
        {
            JsonElement value;
            if (data.ValueKind != JsonValueKind.Object || !data.TryGetProperty(name, out value) ||
                value.ValueKind == JsonValueKind.Null) return fallback;
            try { return value.Deserialize<T>(); }
            catch (JsonException) { return fallback; }
        }
        public static RuntimeStatus Read(string path)
        {
            RuntimeStatus status = new RuntimeStatus();
            if (!File.Exists(path)) return status;
            try
            {
                string json;
                using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (stream.Length > 65536) throw new InvalidDataException("Status file exceeds 64 KiB.");
                    using (StreamReader reader = new StreamReader(stream, Encoding.UTF8)) json = reader.ReadToEnd();
                }
                using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 32 });
                JsonElement data = document.RootElement;
                if (data.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Status must be a JSON object.");
                status.ProcessId = Value<int>(data, "processId", 0);
                status.State = Value<string>(data, "state", "waiting");
                status.InputFps = Value<double>(data, "inputFps", 0);
                status.OutputFps = Value<double>(data, "outputFps", 0);
                status.OutputFrames = Value<long>(data, "outputFrames", 0);
                status.Message = Value<string>(data, "message", "");
                status.Transport = Value<string>(data, "transport", "");
                status.BypassReason = Value<string>(data, "bypassReason", "");
                status.InputRateSelected = Value<bool>(data, "inputRateSelected", true);
                status.InputRateMask = Value<int>(data, "inputRateMask", 63);
                status.DoubleRate = Value<bool>(data, "doubleRate", false);
                status.GpuCorrection = Value<bool?>(data, "gpuMidpointCorrection", null);
                status.AppearanceProtection = Value<bool?>(data, "appearanceProtection", null);
                status.MidpointPassFrames = Value<long>(data, "midpointPassFrames", 0);
                status.AppearancePassFrames = Value<long>(data, "appearancePassFrames", 0);
                status.UpdatedUtc = File.GetLastWriteTimeUtc(path);
                status.Exists = true;
                if (status.ProcessId > 0)
                {
                    try
                    {
                        using (Process process = Process.GetProcessById(status.ProcessId))
                            status.ProcessAlive = !process.HasExited && process.StartTime.ToUniversalTime() <= status.UpdatedUtc;
                    }
                    catch { status.ProcessAlive = false; }
                }
            }
            catch (Exception ex) { status.ReadError = ex.Message; }
            return status;
        }
    }

    public sealed class RegistrationInfo
    {
        private const string FilterClsid = "{EDECA044-78CD-40EB-8F37-63D947C501A0}";
        private const string PageClsid = "{DE1F386E-626B-442A-98C5-59C7713D21BA}";
        public bool PlayerRunning;
        public bool ElevatedPlayer;
        public bool UnknownPlayerElevation;
        public bool UserRegistered;
        public bool MachineRegistered;
        public string PlayerDescription = "실행 중 아님";
        public string RegistrationReadError = "";
        [DllImport("kernel32.dll", SetLastError=true)]
        private static extern IntPtr OpenProcess(uint access, bool inherit, int processId);
        [DllImport("advapi32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetTokenInformation(IntPtr token, int informationClass, out int information, int length, out int returned);
        [DllImport("kernel32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
        public static bool? IsElevated(int processId)
        {
            IntPtr process = OpenProcess(0x1000, false, processId); // PROCESS_QUERY_LIMITED_INFORMATION
            if (process == IntPtr.Zero) return null;
            IntPtr token = IntPtr.Zero;
            try
            {
                if (!OpenProcessToken(process, 0x0008, out token)) return null; // TOKEN_QUERY
                int elevated, returned;
                return GetTokenInformation(token, 20, out elevated, sizeof(int), out returned) ? (bool?)(elevated != 0) : null;
            }
            finally
            {
                if (token != IntPtr.Zero) CloseHandle(token);
                CloseHandle(process);
            }
        }
        private static bool Matches(RegistryKey classes, string clsid, string expectedPath)
        {
            using (RegistryKey key = classes.OpenSubKey(@"CLSID\" + clsid + @"\InprocServer32"))
            {
                string value = key == null ? null : key.GetValue("") as string;
                if (String.IsNullOrEmpty(value)) return false;
                try { return String.Equals(Path.GetFullPath(value.Trim('"')), expectedPath, StringComparison.OrdinalIgnoreCase); }
                catch { return false; }
            }
        }
        public static RegistrationInfo Read(string baseDirectory)
        {
            RegistrationInfo info = new RegistrationInfo();
            List<string> descriptions = new List<string>();
            foreach (Process process in Process.GetProcessesByName("PotPlayerMini64"))
            {
                using (process)
                {
                    try
                    {
                        if (process.HasExited) continue;
                        info.PlayerRunning = true;
                        bool? elevated = IsElevated(process.Id);
                        info.ElevatedPlayer |= elevated == true;
                        info.UnknownPlayerElevation |= !elevated.HasValue;
                        descriptions.Add(process.Id + (elevated == true ? " (관리자 권한)" : elevated == false ? " (일반 권한)" : " (권한 확인 불가)"));
                    }
                    catch { info.UnknownPlayerElevation = true; }
                }
            }
            if (descriptions.Count > 0) info.PlayerDescription = String.Join(", ", descriptions.ToArray());
            string expected = Path.GetFullPath(Path.Combine(baseDirectory, "NvofPotPlayer.ax"));
            foreach (RegistryHive hive in new RegistryHive[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
            {
                try
                {
                    using (RegistryKey root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64))
                    using (RegistryKey classes = root.OpenSubKey(@"Software\Classes"))
                    {
                        bool registered = classes != null && Matches(classes, FilterClsid, expected) && Matches(classes, PageClsid, expected);
                        if (hive == RegistryHive.CurrentUser) info.UserRegistered = registered;
                        else info.MachineRegistered = registered;
                    }
                }
                catch (Exception ex) { info.RegistrationReadError = ex.Message; }
            }
            return info;
        }
        public static ProcessStartInfo CreateStartInfo(string baseDirectory, bool machine)
        {
            ProcessStartInfo start = new ProcessStartInfo(Path.Combine(baseDirectory, "NvofRegister.exe"), machine ? "--register-machine" : "--register");
            start.WorkingDirectory = baseDirectory;
            if (machine)
            {
                start.UseShellExecute = true;
                start.Verb = "runas";
                start.WindowStyle = ProcessWindowStyle.Hidden;
            }
            else
            {
                start.UseShellExecute = false;
                start.CreateNoWindow = true;
                start.RedirectStandardOutput = true;
                start.RedirectStandardError = true;
            }
            return start;
        }
    }

    public sealed class Controller
    {
        private readonly Window window;
        private readonly string baseDirectory;
        private readonly string statusDirectory;
        private readonly SettingsStore settings;
        private readonly DispatcherTimer timer;
        private bool loading;
        private bool preview;
        private string diagnostics = "";
        private string lastActionError = "";
        private ToggleButton enabled, gpuCorrection, appearanceProtection;
        private readonly CheckBox[] inputRates = new CheckBox[6];
        private TextBlock rateSelectionHint, outputRateHint;
        private TextBlock notice, statusTitle, statusDetail, registerNotice;
        private System.Windows.Shapes.Ellipse statusDot;
        private TextBox diagnosticsText;
        private Button registerButton;
        private RegistrationInfo registration;
        private bool registering;
        private string registrationResult;
        private bool registrationResultError;

        public Controller(Window window, bool preview)
        {
            this.window = window;
            this.preview = preview;
            baseDirectory = AppContext.BaseDirectory;
            statusDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NvofPotPlayer");
            settings = new SettingsStore(Path.Combine(baseDirectory, "NvofPotPlayer.ini"));
            enabled = Find<ToggleButton>("EnabledToggle");
            gpuCorrection = Find<ToggleButton>("GpuCorrectionToggle");
            appearanceProtection = Find<ToggleButton>("AppearanceProtectionToggle");
            outputRateHint = Find<TextBlock>("OutputRateHint");
            for (int index = 0; index < inputRates.Length; index++)
                inputRates[index] = Find<CheckBox>("InputRate" + index);
            rateSelectionHint = Find<TextBlock>("RateSelectionHint");
            notice = Find<TextBlock>("SaveNotice");
            statusTitle = Find<TextBlock>("StatusTitle");
            statusDetail = Find<TextBlock>("StatusDetail");
            statusDot = Find<System.Windows.Shapes.Ellipse>("StatusDot");
            diagnosticsText = Find<TextBox>("DiagnosticsText");
            registerNotice = Find<TextBlock>("RegisterNotice");
            registerButton = Find<Button>("RegisterButton");
            LoadSettings();
            Find<FrameworkElement>("NativeEnhancementPanel").Visibility = settings.NativeSynthesis ? Visibility.Visible : Visibility.Collapsed;
            Find<FrameworkElement>("LegacyEnhancementPanel").Visibility = settings.NativeSynthesis ? Visibility.Collapsed : Visibility.Visible;
            Find<Button>("InstallFolderButton").Click += delegate {
                try { Process.Start(new ProcessStartInfo(baseDirectory) { UseShellExecute=true }); }
                catch (Exception ex) { Notify("설치 폴더를 열지 못했습니다.", true, ex.Message); }
            };
            Find<Button>("UninstallButton").Click += delegate {
                if (preview) return;
                string uninstaller = FindUninstaller(baseDirectory);
                if (!File.Exists(uninstaller)) { Notify("설치 프로그램으로 설치한 버전에서 제거할 수 있습니다.", true); return; }
                try { Process.Start(new ProcessStartInfo(uninstaller) { UseShellExecute=true, WorkingDirectory=baseDirectory }); window.Close(); }
                catch (Exception ex) { Notify("제거 프로그램을 열지 못했습니다.", true, ex.Message); }
            };
            if (!preview)
            {
                try { settings.MigrateToDoubleRate(); }
                catch (Exception ex) { Notify("설정 정리에 실패했습니다. 폴더 쓰기 권한을 확인하세요.", true, ex.Message); }
            }
            enabled.Click += delegate { SaveEnabled(); };
            gpuCorrection.Click += delegate { SaveEnhancement(true); };
            appearanceProtection.Click += delegate { SaveEnhancement(false); };
            foreach (CheckBox rate in inputRates) rate.Click += delegate { SaveInputRates(); };
            Find<Button>("CloseButton").Click += delegate { window.Close(); };
            Find<Button>("LaunchButton").Click += delegate { LaunchPlayer(); };
            registerButton.Click += delegate { RegisterFilter(); };
            Find<Button>("LogButton").Click += delegate { OpenDiagnostics(); };
            Find<Button>("CopyButton").Click += delegate
            {
                try { Clipboard.SetText(diagnostics); Notify("진단 정보를 복사했습니다.", false); }
                catch (Exception ex) { Notify("복사하지 못했습니다. 다시 시도하세요.", true, ex.Message); }
            };
            RefreshStatus();
            timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            timer.Tick += delegate { RefreshStatus(); };
            if (!preview) timer.Start();
            window.Closed += delegate { timer.Stop(); };
        }
        internal static string FindUninstaller(string directory)
        {
            // Inno may allocate unins001.exe after an immediate reinstall.
            // Prefer this product's registry entry, but never launch another folder.
            string expected = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar);
            const string key = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\{4932901D-91E6-4FE4-A79E-D63258D637F2}_is1";
            foreach (RegistryHive hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
            {
                try
                {
                    using (RegistryKey root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64))
                    using (RegistryKey installed = root.OpenSubKey(key))
                    {
                        string command = installed == null ? null : installed.GetValue("UninstallString") as string;
                        if (!String.IsNullOrWhiteSpace(command))
                        {
                            string path = command.Trim().Trim('"');
                            if (ValidLocalUninstaller(expected, path)) return Path.GetFullPath(path);
                        }
                    }
                }
                catch (Exception ex) when (ex is System.Security.SecurityException || ex is UnauthorizedAccessException || ex is ArgumentException || ex is IOException) { }
            }
            if (!Directory.Exists(expected)) return null;
            string[] candidates = Directory.GetFiles(expected, "unins???.exe", SearchOption.TopDirectoryOnly);
            Array.Sort(candidates, StringComparer.OrdinalIgnoreCase);
            for (int i=candidates.Length-1; i>=0; --i)
                if (ValidLocalUninstaller(expected, candidates[i])) return candidates[i];
            return null;
        }
        internal static bool ValidLocalUninstaller(string directory, string path)
        {
            try
            {
                string full = Path.GetFullPath(path);
                string name = Path.GetFileName(full);
                return String.Equals(Path.GetDirectoryName(full), Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase)
                    && name.Length==12 && name.StartsWith("unins",StringComparison.OrdinalIgnoreCase)
                    && name.EndsWith(".exe",StringComparison.OrdinalIgnoreCase)
                    && Char.IsDigit(name[5]) && Char.IsDigit(name[6]) && Char.IsDigit(name[7])
                    && File.Exists(full) && File.Exists(Path.ChangeExtension(full,".dat"));
            }
            catch (Exception ex) when (ex is ArgumentException || ex is NotSupportedException || ex is PathTooLongException) { return false; }
        }
        private T Find<T>(string name) where T : class { return (T)window.FindName(name); }
        private Brush Color(string hex) { return (Brush)new BrushConverter().ConvertFromString(hex); }
        private void LoadSettings()
        {
            loading = true;
            enabled.IsChecked = settings.Enabled;
            gpuCorrection.IsChecked = settings.GpuCorrection;
            appearanceProtection.IsChecked = settings.AppearanceProtection;
            UpdateOutputHint();
            int mask = settings.InputRateMask;
            for (int index = 0; index < inputRates.Length; index++)
                inputRates[index].IsChecked = (mask & (1 << index)) != 0;
            UpdateRateHint();
            loading = false;
        }
        private void SaveEnabled()
        {
            if (loading || preview) return;
            try { settings.SetEnabled(enabled.IsChecked == true); Notify("저장됨 · 영상을 다시 열면 적용됩니다.", false); }
            catch (Exception ex) { LoadSettings(); Notify("설정을 저장하지 못했습니다. 폴더 쓰기 권한을 확인하세요.", true, ex.Message); }
        }
        private void SaveEnhancement(bool gpu)
        {
            if (loading || preview) return;
            try
            {
                if (gpu) settings.SetGpuCorrection(gpuCorrection.IsChecked == true);
                else settings.SetAppearanceProtection(appearanceProtection.IsChecked == true);
                Notify("저장됨 · 영상을 다시 열면 적용됩니다.", false);
            }
            catch (Exception ex) { LoadSettings(); Notify("설정을 저장하지 못했습니다. 폴더 쓰기 권한을 확인하세요.", true, ex.Message); }
        }
        private static string Choice(bool? value) { return value.HasValue ? (value.Value ? "켜짐" : "꺼짐") : "이전 필터 · 보고 없음"; }
        private int SelectedRateMask()
        {
            int mask = 0;
            for (int index = 0; index < inputRates.Length; index++)
                if (inputRates[index].IsChecked == true) mask |= 1 << index;
            return mask;
        }
        private void UpdateRateHint()
        {
            rateSelectionHint.Text = SelectedRateMask() == 0
                ? "선택한 항목이 없어 모든 영상을 원본 프레임으로 재생합니다."
                : "23.976·29.97·59.94 fps는 각각 24·30·60에 포함됩니다.";
        }
        private void SaveInputRates()
        {
            UpdateRateHint();
            if (loading || preview) return;
            try { settings.SetInputRateMask(SelectedRateMask()); Notify("저장됨 · 영상을 다시 열면 적용됩니다.", false); }
            catch (Exception ex) { LoadSettings(); Notify("설정을 저장하지 못했습니다. 폴더 쓰기 권한을 확인하세요.", true, ex.Message); }
        }
        private void UpdateOutputHint()
        {
            outputRateHint.Text = "23.976 → 47.952 fps · 30 → 60 fps";
        }
        private void Notify(string text, bool error, string detail = null)
        {
            notice.Text = text;
            notice.Foreground = Color(error ? "#A12D2D" : "#216D4C");
            lastActionError = error ? text + (String.IsNullOrEmpty(detail) ? "" : "\n" + detail) : "";
            notice.ToolTip = error ? lastActionError : null;
            System.Windows.Automation.AutomationProperties.SetHelpText(notice, lastActionError);
        }
        private string Fps(double value)
        {
            return double.IsNaN(value) || double.IsInfinity(value) || value <= 0 ? "—" : value.ToString("0.###", CultureInfo.InvariantCulture);
        }
        public void RefreshStatus()
        {
            registration = RegistrationInfo.Read(baseDirectory);
            RefreshRegistrationNotice();
            RuntimeStatus status = RuntimeStatus.Read(Path.Combine(statusDirectory, "status.json"));
            ShowStatus(status);
        }
        private void ShowStatus(RuntimeStatus status)
        {
            statusDot.Fill = Color("#717C74");
            bool recent = status.Exists && DateTime.UtcNow - status.UpdatedUtc <= TimeSpan.FromSeconds(5);
            if (!status.Exists)
            {
                statusTitle.Text = "팟플레이어 연결 대기";
                statusDetail.Text = "필터를 연결한 뒤 영상을 재생하면 상태가 표시됩니다.";
            }
            else if (!status.ProcessAlive || status.State == "stopped")
            {
                statusTitle.Text = "재생 종료";
                statusDetail.Text = "다음 영상을 재생하면 필터 상태가 표시됩니다.";
            }
            else if (status.State == "error")
            {
                statusTitle.Text = "보간 필터 오류";
                statusDetail.Text = "진단 정보를 확인한 뒤 영상을 다시 열어 주세요.";
                statusDot.Fill = Color("#A12D2D");
            }
            else if (status.State == "active")
            {
                statusTitle.Text = recent ? "프레임 보간 동작 중" : "마지막 재생 상태 · 보간";
                statusDetail.Text = Fps(status.InputFps) + " → " + Fps(status.OutputFps) + " fps" + (recent ? " · NVIDIA Optical Flow" : " · 현재 재생 상태는 업데이트 대기 중입니다.");
                statusDot.Fill = Color(recent ? "#216D4C" : "#717C74");
            }
            else if (status.State == "bypass")
            {
                if (status.BypassReason == "source-rate")
                {
                    statusTitle.Text = recent ? "보간 대상에서 제외된 영상" : "마지막 재생 상태 · 보간 대상에서 제외";
                    statusDetail.Text = Fps(status.InputFps) + " fps 원본으로 재생" + (recent ? " 중입니다. 보간할 원본 프레임 선택에서 제외되어 있습니다." : "했습니다. 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "at-or-above-target")
                {
                    statusTitle.Text = recent ? "출력 FPS 이상 · 원본 재생" : "마지막 재생 상태 · 출력 FPS 이상";
                    statusDetail.Text = Fps(status.InputFps) + " fps 영상은 출력 FPS 이상이므로 원본 프레임을 유지합니다." + (recent ? "" : " 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "unknown-source-rate")
                {
                    statusTitle.Text = recent ? "원본 FPS 확인 불가 · 원본 재생" : "마지막 재생 상태 · 원본 FPS 확인 불가";
                    statusDetail.Text = "원본 영상의 FPS를 확인할 수 없어 ×2 보간을 적용하지 않습니다." + (recent ? "" : " 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "output-rate-unsupported")
                {
                    statusTitle.Text = recent ? "지원 범위 밖 · 원본 재생" : "마지막 재생 상태 · 지원 범위 밖";
                    statusDetail.Text = "두 배로 늘린 FPS가 지원 범위를 벗어나 원본 프레임을 유지합니다." + (recent ? "" : " 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "disabled")
                {
                    statusTitle.Text = recent ? "프레임 보간 꺼짐" : "마지막 재생 상태 · 보간 꺼짐";
                    statusDetail.Text = recent ? "프레임 보간 사용이 꺼져 있어 원본 프레임으로 재생합니다." : "원본 프레임으로 재생했습니다. 현재 재생 상태는 업데이트 대기 중입니다.";
                }
                else
                {
                    statusTitle.Text = recent ? "원본 프레임으로 재생 중" : "마지막 재생 상태 · 원본";
                    statusDetail.Text = "보간을 적용하지 않는 상태입니다. 자세한 이유는 진단 정보에서 확인할 수 있습니다.";
                }
            }
            else
            {
                statusTitle.Text = "필터 연결됨 · 프레임 대기";
                statusDetail.Text = "영상을 재생하면 실제 출력 프레임이 표시됩니다.";
            }
            if (!String.IsNullOrEmpty(status.ReadError))
            {
                statusTitle.Text = "상태 정보 읽기 대기";
                statusDetail.Text = "아직 필터 상태를 읽지 못했습니다. 잠시 후 자동으로 다시 확인합니다.";
            }
            if (registration != null && registration.ElevatedPlayer && !registration.MachineRegistered &&
                (!status.Exists || !status.ProcessAlive || status.State == "stopped"))
            {
                statusTitle.Text = "관리자 팟플레이어 연결 준비";
                statusDetail.Text = "팟플레이어가 관리자 권한으로 실행 중입니다. 연결 설정에서 관리자용 필터를 등록하세요.";
            }
            StringBuilder text = new StringBuilder();
            text.AppendLine("NVOF for PotPlayer 0.3.1-opt.1");
            if (registration != null)
            {
                text.AppendLine("팟플레이어 권한: " + registration.PlayerDescription);
                text.AppendLine("사용자별 필터·설정 창 등록: " + (registration.UserRegistered ? "이 폴더 등록됨" : "없음 또는 다른 폴더"));
                text.AppendLine("관리자용 필터·설정 창 등록: " + (registration.MachineRegistered ? "이 폴더 등록됨" : "없음 또는 다른 폴더"));
                text.AppendLine("현재 등록 버튼: " + (registration.ElevatedPlayer ? "관리자용 (Windows 승인 필요)" : "현재 사용자용"));
                if (registration.ElevatedPlayer && !registration.MachineRegistered) text.AppendLine("관리자 팟플레이어에서는 사용자별 COM 등록을 사용하지 않습니다. 관리자용 등록이 필요합니다.");
                if (!String.IsNullOrEmpty(registration.RegistrationReadError)) text.AppendLine("등록 조회 오류: " + registration.RegistrationReadError);
            }
            text.AppendLine("설정: " + (enabled.IsChecked == true ? "사용" : "사용 안 함") + " / " + "원본의 두 배 (×2)");
            string[] rateLabels = { "24", "25", "30", "50", "60", "기타" };
            List<string> selectedLabels = new List<string>();
            for (int index = 0; index < inputRates.Length; index++)
                if (inputRates[index].IsChecked == true) selectedLabels.Add(rateLabels[index]);
            text.AppendLine("보간할 원본 프레임: " + (selectedLabels.Count == 0 ? "선택 없음" : String.Join(", ", selectedLabels.ToArray()) + " fps"));
            text.AppendLine("저장된 GPU 보정: " + Choice(gpuCorrection.IsChecked));
            text.AppendLine("저장된 형태 변화 보호: " + Choice(appearanceProtection.IsChecked));
            text.AppendLine("영상 전달: " + status.TransportLabel);
            text.AppendLine("필터: " + (File.Exists(Path.Combine(baseDirectory, "NvofPotPlayer.ax")) ? "설치 파일 있음" : "설치 파일 없음"));
            text.AppendLine("상태: " + (status.Exists ? status.State : "아직 보고 없음"));
            if (status.Exists)
            {
                text.AppendLine("프로세스: " + status.ProcessId + (status.ProcessAlive ? " (실행 중)" : " (종료됨)"));
                text.AppendLine("FPS: " + Fps(status.InputFps) + " → " + Fps(status.OutputFps));
                text.AppendLine("출력 프레임: " + status.OutputFrames);
                text.AppendLine("현재 세션 GPU 보정 옵션: " + Choice(status.GpuCorrection));
                text.AppendLine("현재 세션 형태 변화 보호 옵션: " + Choice(status.AppearanceProtection));
                if (status.Transport == "d3d11-gpu") text.AppendLine("GPU 보정 / 형태 변화 보호 단계 실행 프레임: " + status.MidpointPassFrames + " / " + status.AppearancePassFrames + " (픽셀별 적용 여부와는 다름)");
                else text.AppendLine("추가 보정은 GPU 직접 전달 경로에서만 동작합니다.");
                text.AppendLine("재생에 적용된 원본 프레임 선택: " + status.InputRateMask);
                text.AppendLine("재생에 적용된 출력 모드: " + (status.DoubleRate ? "원본의 두 배 (×2)" : "고정 출력 FPS"));
                if (!String.IsNullOrEmpty(status.BypassReason)) text.AppendLine("원본 재생 이유: " + (status.BypassReason == "source-rate" ? "원본 프레임 선택에서 제외" : status.BypassReason == "disabled" ? "보간 사용 꺼짐" : status.BypassReason == "at-or-above-target" ? "출력 FPS 이상" : status.BypassReason == "unknown-source-rate" ? "원본 FPS 확인 불가" : status.BypassReason == "output-rate-unsupported" ? "두 배 출력 FPS가 지원 범위 밖" : status.BypassReason));
                text.AppendLine("마지막 기록: " + status.UpdatedUtc.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss"));
                if (!String.IsNullOrEmpty(status.Message)) text.AppendLine("필터 메시지: " + status.Message);
            }
            if (!String.IsNullOrEmpty(status.ReadError)) text.AppendLine("읽기 오류: " + status.ReadError);
            if (!String.IsNullOrEmpty(lastActionError)) text.AppendLine("설정창 작업 오류: " + lastActionError);
            text.AppendLine(status.TransportDetail);
            text.AppendLine("진단 폴더: " + statusDirectory);
            string value = text.ToString();
            if (value != diagnostics) { diagnostics = value; diagnosticsText.Text = value; }
        }
        private void RefreshRegistrationNotice()
        {
            bool filesExist = File.Exists(Path.Combine(baseDirectory, "NvofRegister.exe")) && File.Exists(Path.Combine(baseDirectory, "NvofPotPlayer.ax"));
            registerButton.IsEnabled = filesExist && !registering;
            registerButton.Content = registration != null && registration.ElevatedPlayer ? "관리자용 필터 등록" : "필터 등록";
            if (registering) return;
            registerNotice.Foreground = Color(registrationResultError ? "#A12D2D" : "#59635D");
            if (!filesExist) registerNotice.Text = "같은 폴더에 NvofRegister.exe와 NvofPotPlayer.ax가 필요합니다.";
            else if (registrationResult != null) registerNotice.Text = registrationResult;
            else if (registration != null && registration.ElevatedPlayer)
                registerNotice.Text = registration.MachineRegistered
                    ? "관리자용 필터와 설정 창이 등록돼 있습니다. 다시 등록하려면 Windows 관리자 승인이 필요합니다."
                    : "팟플레이어가 관리자 권한으로 실행 중이라 사용자별 등록을 읽지 못합니다. 관리자용 등록 버튼을 누르면 Windows 승인창이 열립니다.";
            else if (registration != null && registration.UnknownPlayerElevation)
                registerNotice.Text = "팟플레이어 실행 권한을 확인하지 못했습니다. 현재 사용자용으로 등록합니다.";
            else if (registration != null && registration.UserRegistered)
                registerNotice.Text = "현재 사용자용 필터와 설정 창이 등록돼 있습니다. 필요하면 다시 등록할 수 있습니다.";
            else registerNotice.Text = "현재 Windows 사용자에게만 등록합니다.";
        }
        private async void RegisterFilter()
        {
            if (preview || registering) return;
            registration = RegistrationInfo.Read(baseDirectory);
            bool machine = registration.ElevatedPlayer;
            registering = true;
            registrationResult = null;
            registrationResultError = false;
            RefreshRegistrationNotice();
            registerNotice.Text = machine
                ? "관리자 팟플레이어에 연결하려면 관리자용 등록이 필요합니다. Windows 승인창에서 허용해 주세요."
                : "현재 사용자용 필터를 등록하는 중입니다…";
            try
            {
                await Task.Run(delegate
                {
                    using (Process process = Process.Start(RegistrationInfo.CreateStartInfo(baseDirectory, machine)))
                    {
                        if (process == null) throw new InvalidOperationException("등록 작업을 시작했는지 확인하지 못했습니다.");
                        Task<string> output = machine ? null : process.StandardOutput.ReadToEndAsync();
                        Task<string> error = machine ? null : process.StandardError.ReadToEndAsync();
                        if (!process.WaitForExit(30000)) throw new TimeoutException("등록 작업이 아직 완료되지 않았습니다. 완료 여부를 확인한 뒤 다시 시도하세요.");
                        if (!machine) Task.WaitAll(output, error);
                        if (process.ExitCode != 0)
                        {
                            string detail = machine ? "등록 도구 종료 코드: " + process.ExitCode : (error.Result + " " + output.Result).Trim();
                            throw new InvalidOperationException(detail);
                        }
                    }
                    RegistrationInfo verified = RegistrationInfo.Read(baseDirectory);
                    if (!(machine ? verified.MachineRegistered : verified.UserRegistered))
                        throw new InvalidOperationException("등록 결과를 확인하지 못했습니다. 진단 정보의 등록 상태를 확인하세요.");
                });
                registrationResult = machine
                    ? "관리자용 필터와 설정 창을 등록했습니다. 팟플레이어를 완전히 종료한 뒤 다시 실행하세요."
                    : "현재 사용자용 필터와 설정 창을 등록했습니다. 팟플레이어에서 필터를 추가하거나 다시 실행하세요.";
            }
            catch (System.ComponentModel.Win32Exception ex)
            {
                registrationResultError = true;
                registrationResult = ex.NativeErrorCode == 1223
                    ? "Windows 관리자 승인이 취소되어 등록하지 않았습니다. 다시 시도하거나 팟플레이어를 일반 권한으로 실행하세요."
                    : "등록하지 못했습니다. " + ex.Message;
            }
            catch (Exception ex)
            {
                registrationResultError = true;
                registrationResult = "등록을 완료했는지 확인하지 못했습니다. " + ex.Message;
            }
            finally
            {
                registering = false;
                RefreshStatus();
            }
        }
        private void LaunchPlayer()
        {
            if (preview) return;
            try
            {
                string path = FindPotPlayer();
                if (path == null)
                {
                    OpenFileDialog dialog = new OpenFileDialog { Title="팟플레이어 실행 파일 선택", Filter="PotPlayer x64|PotPlayerMini64.exe|실행 파일|*.exe", CheckFileExists=true };
                    if (dialog.ShowDialog(window) != true) return;
                    path = dialog.FileName;
                }
                Process.Start(new ProcessStartInfo(path) { UseShellExecute=true, WorkingDirectory=Path.GetDirectoryName(path) });
            }
            catch (Exception ex) { Notify("팟플레이어를 열지 못했습니다. 연결 설정을 확인하세요.", true, ex.Message); }
        }
        private static string FindPotPlayer()
        {
            foreach (RegistryKey root in new RegistryKey[] { Registry.CurrentUser, Registry.LocalMachine })
            {
                using (RegistryKey key = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\PotPlayerMini64.exe"))
                {
                    string path = key == null ? null : key.GetValue("") as string;
                    if (!String.IsNullOrEmpty(path) && File.Exists(path.Trim('"'))) return path.Trim('"');
                }
            }
            string standard = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), @"DAUM\PotPlayer\PotPlayerMini64.exe");
            return File.Exists(standard) ? standard : null;
        }
        private void OpenDiagnostics()
        {
            if (preview) return;
            try
            {
                Directory.CreateDirectory(statusDirectory);
                Process.Start(new ProcessStartInfo(statusDirectory) { UseShellExecute=true });
            }
            catch (Exception ex) { Notify("진단 폴더를 열지 못했습니다.", true, ex.Message); }
        }
        public void PreviewState(string state)
        {
            if (state == "live") return;
            RuntimeStatus fixture = new RuntimeStatus { Exists=state != "waiting", ProcessId=1234, State=state, InputFps=23.976, OutputFps=47.952, DoubleRate=true, OutputFrames=120, UpdatedUtc=DateTime.UtcNow, ProcessAlive=true, Message="Preview fixture — not runtime telemetry." };
            if (state == "error") fixture.Message = "Preview: GPU initialization failed.";
            if (state == "double" || state == "double-expanded")
            {
                fixture.State = "active"; fixture.OutputFps = 47.952; fixture.Transport = "d3d11-gpu"; fixture.DoubleRate = true;
            }
            if (state == "excluded" || state == "disabled" || state == "above-target")
            {
                fixture.State = "bypass";
                fixture.BypassReason = state == "excluded" ? "source-rate" : state == "disabled" ? "disabled" : "at-or-above-target";
                fixture.InputFps = 59.94; fixture.OutputFps = 59.94;
                fixture.Transport = "d3d11-gpu";
                fixture.InputRateSelected = state != "excluded";
                if (state == "excluded") { inputRates[4].IsChecked = false; fixture.InputRateMask = SelectedRateMask(); }
                else if (state == "disabled") enabled.IsChecked = false;
            }
            if (state == "unknown-rate" || state == "unsupported-rate")
            {
                fixture.State = "bypass"; fixture.DoubleRate = true;
                fixture.BypassReason = state == "unknown-rate" ? "unknown-source-rate" : "output-rate-unsupported";
                fixture.InputFps = state == "unknown-rate" ? 0 : 600;
                fixture.OutputFps = fixture.InputFps;
            }
            ShowStatus(fixture);
            notice.Text = "미리보기 · 상태 예시 (실제 실행 기록이 아닙니다)";
            if (state == "error")
            {
                Notify("미리보기 · 저장 실패. 폴더 쓰기 권한을 확인하세요.", true,
                    @"Preview fixture: Access to a deliberately long example path F:\ExampleFolder\AnotherLongExampleFolder\NvofPotPlayer\NvofPotPlayer.ini was denied. Additional diagnostic details remain available in the diagnostics tab.");
                ShowStatus(fixture);
            }
            if (state == "setup") Find<TabItem>("SetupTab").IsSelected = true;
            if (state == "enhancements") Find<TabItem>("EnhancementsTab").IsSelected = true;
            if (state == "expanded" || state == "double-expanded") { Find<TabItem>("SetupTab").IsSelected = true; Find<TabItem>("DiagnosticsTab").IsSelected = true; }
        }
    }

    internal static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            try
            {
                if (Array.IndexOf(args, "--self-test") >= 0)
                {
                    int result = SelfTest();
                    string runtimeReport = Arg(args, "--runtime-info", "");
                    if (!String.IsNullOrEmpty(runtimeReport)) WriteRuntimeInfo(runtimeReport);
                    return result;
                }
                Application application = new Application { ShutdownMode=ShutdownMode.OnMainWindowClose };
                Window window;
                using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("MainWindow.xaml"))
                    window = (Window)XamlReader.Load(stream);
                int previewIndex = Array.IndexOf(args, "--render-preview");
                bool preview = previewIndex >= 0;
                Controller controller = new Controller(window, preview);
                if (preview)
                {
                    if (previewIndex + 1 >= args.Length) throw new ArgumentException("--render-preview requires an output path.");
                    string state = Arg(args, "--preview-state", "live");
                    int width = Int32.Parse(Arg(args, "--preview-width", "504"), CultureInfo.InvariantCulture);
                    int height = Int32.Parse(Arg(args, "--preview-height", "461"), CultureInfo.InvariantCulture);
                    if (width < 360 || width > 3000 || height < 300 || height > 3000) throw new ArgumentOutOfRangeException("preview size");
                    controller.PreviewState(state);
                    string focusedName = Arg(args, "--preview-focus", "");
                    if (!String.IsNullOrEmpty(focusedName))
                    {
                        Control focused = window.FindName(focusedName) as Control;
                        if (focused == null || !focused.Focusable) throw new ArgumentException("Unknown focus target.");
                        System.Windows.Input.FocusManager.SetFocusedElement(window, focused);
                    }
                    FrameworkElement content = (FrameworkElement)window.Content;
                    content.Measure(new Size(width, height));
                    content.Arrange(new Rect(0, 0, width, height));
                    content.UpdateLayout();
                    double scale = Double.Parse(Arg(args, "--preview-scale", "1"), CultureInfo.InvariantCulture);
                    if (scale < 1 || scale > 3) throw new ArgumentOutOfRangeException("preview scale");
                    RenderTargetBitmap bitmap = new RenderTargetBitmap((int)Math.Ceiling(width * scale), (int)Math.Ceiling(height * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
                    bitmap.Render(content);
                    string output = Path.GetFullPath(args[previewIndex + 1]);
                    Directory.CreateDirectory(Path.GetDirectoryName(output));
                    using (FileStream stream = File.Create(output))
                    {
                        PngBitmapEncoder encoder = new PngBitmapEncoder();
                        encoder.Frames.Add(BitmapFrame.Create(bitmap));
                        encoder.Save(stream);
                    }
                    return 0;
                }
                application.Run(window);
                return 0;
            }
            catch (Exception ex)
            {
                string report = ex.ToString();
                try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "NvofControl-error.txt"), report); } catch { }
                if (Array.IndexOf(args, "--render-preview") < 0 && Array.IndexOf(args, "--self-test") < 0)
                    MessageBox.Show("설정창을 열지 못했습니다.\n" + ex.Message, "NVOF", MessageBoxButton.OK, MessageBoxImage.Error);
                return 1;
            }
        }
        private static string Arg(string[] args, string name, string fallback)
        {
            int index = Array.IndexOf(args, name);
            return index >= 0 && index + 1 < args.Length ? args[index + 1] : fallback;
        }
        private static void WriteRuntimeInfo(string path)
        {
            Dictionary<string, string> modules = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            using (Process process = Process.GetCurrentProcess())
                foreach (ProcessModule module in process.Modules)
                    if (module.ModuleName.Equals("coreclr.dll", StringComparison.OrdinalIgnoreCase) ||
                        module.ModuleName.Equals("hostpolicy.dll", StringComparison.OrdinalIgnoreCase) ||
                        module.ModuleName.Equals("hostfxr.dll", StringComparison.OrdinalIgnoreCase) ||
                        module.ModuleName.Equals("wpfgfx_cor3.dll", StringComparison.OrdinalIgnoreCase))
                        modules[module.ModuleName] = module.FileName;
            var report = new
            {
                framework = RuntimeInformation.FrameworkDescription,
                runtimeVersion = Environment.Version.ToString(),
                processArchitecture = RuntimeInformation.ProcessArchitecture.ToString(),
                baseDirectory = AppContext.BaseDirectory,
                wpfAssembly = typeof(Window).Assembly.Location,
                jsonAssembly = typeof(JsonDocument).Assembly.Location,
                modules = modules
            };
            string output = Path.GetFullPath(path);
            Directory.CreateDirectory(Path.GetDirectoryName(output));
            File.WriteAllText(output, JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
        }
        private static int SelfTest()
        {
            string folder = Path.Combine(Path.GetTempPath(), "NvofControl-test-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            try
            {
                string settingsPath = Path.Combine(folder, "settings.ini");
                File.WriteAllText(settingsPath, "[Nvof]\r\nNativeD3D11=1\r\nProbeServices=0\r\n");
                SettingsStore store = new SettingsStore(settingsPath);
                if (store.InputRateMask != 63) throw new Exception("Missing source-rate selection must preserve all-rates behavior.");
                if (!store.GpuCorrection || !store.AppearanceProtection) throw new Exception("Existing INIs must retain both enhancements by default.");
                foreach (bool gpu in new bool[] {false,true}) foreach (bool mouth in new bool[] {false,true})
                {
                    store.SetGpuCorrection(gpu); store.SetAppearanceProtection(mouth); store.SetEnabled(false); store.MigrateToDoubleRate();
                    if (store.GpuCorrection != gpu || store.AppearanceProtection != mouth) throw new Exception("Independent enhancement settings were coupled or migration reset them.");
                }
                foreach (int oldTarget in new int[] { 60, 120 })
                {
                    File.WriteAllText(settingsPath, "[Nvof]\r\nNativeD3D11=1\r\nProbeServices=0\r\nEnabled=0\r\nInputRateMask=5\r\nTargetFps=" + oldTarget + "\r\nDoubleRate=0\r\n");
                    store.MigrateToDoubleRate();
                    string migrated = File.ReadAllText(settingsPath);
                    if (store.Enabled || store.InputRateMask != 5 || migrated.Contains("TargetFps=") || !migrated.Contains("DoubleRate=1")) throw new Exception("Legacy fixed output migration failed.");
                    if (!migrated.Contains("NativeD3D11=1") || !migrated.Contains("ProbeServices=0")) throw new Exception("Migration changed native transport preferences.");
                    store.MigrateToDoubleRate();
                    if (File.ReadAllText(settingsPath) != migrated) throw new Exception("Migration must be idempotent.");
                }
                store.SetInputRateMask(0);
                if (store.InputRateMask != 0) throw new Exception("Selecting no source rates must persist.");
                store.SetInputRateMask(63); store.SetEnabled(true);
                if (!store.Enabled || store.InputRateMask != 63) throw new Exception("Settings round-trip failed.");
                string uninstallFixture=Path.Combine(folder,"uninstaller");
                Directory.CreateDirectory(uninstallFixture);
                File.WriteAllText(Path.Combine(uninstallFixture,"unins000.exe"),"old");
                File.WriteAllText(Path.Combine(uninstallFixture,"unins000.dat"),"old");
                File.WriteAllText(Path.Combine(uninstallFixture,"unins001.exe"),"new");
                File.WriteAllText(Path.Combine(uninstallFixture,"unins001.dat"),"new");
                File.WriteAllText(Path.Combine(uninstallFixture,"unins999.exe"),"orphan");
                if(Controller.FindUninstaller(uninstallFixture)!=Path.Combine(uninstallFixture,"unins001.exe")) throw new Exception("Reinstall uninstaller selection failed.");
                if(Controller.ValidLocalUninstaller(folder,Path.Combine(uninstallFixture,"unins001.exe"))) throw new Exception("Foreign-folder uninstaller accepted.");
                if(Controller.ValidLocalUninstaller(uninstallFixture,Path.Combine(uninstallFixture,"unins001.exe")+" /silent")) throw new Exception("Uninstall command arguments accepted as a local executable.");
                string statusPath = Path.Combine(folder, "status.json");
                if (RuntimeStatus.Read(statusPath).Exists) throw new Exception("Missing status must be inactive.");
                File.WriteAllText(statusPath, "{\"processId\":" + Process.GetCurrentProcess().Id + ",\"state\":\"active\",\"inputFps\":23.976,\"outputFps\":59.94,\"outputFrames\":12,\"message\":\"OK\"}");
                RuntimeStatus status = RuntimeStatus.Read(statusPath);
                if (!status.Exists || !status.ProcessAlive || status.State != "active" || status.OutputFrames != 12 || Math.Abs(status.OutputFps-59.94)>0.001) throw new Exception("Status parsing failed.");
                File.WriteAllText(statusPath, "{\"processId\":" + Process.GetCurrentProcess().Id + ",\"state\":\"bypass\",\"bypassReason\":\"source-rate\",\"inputRateSelected\":false,\"inputRateMask\":5,\"inputFps\":59.94,\"outputFps\":59.94}");
                RuntimeStatus excluded = RuntimeStatus.Read(statusPath);
                if (excluded.BypassReason != "source-rate" || excluded.InputRateSelected || excluded.InputRateMask != 5 || excluded.InputFps != excluded.OutputFps) throw new Exception("Excluded-rate status must retain its reason and original rate.");
                File.WriteAllText(statusPath, "{broken");
                status = RuntimeStatus.Read(statusPath);
                if (status.Exists || String.IsNullOrEmpty(status.ReadError)) throw new Exception("Invalid status must not show active.");
                RuntimeStatus gpuTransport = new RuntimeStatus { Transport="d3d11-gpu" };
                if (gpuTransport.TransportLabel != "GPU 메모리로 직접 전달" || gpuTransport.TransportDetail.Contains("RAM")) throw new Exception("GPU transport must not display a CPU-memory claim.");
                RuntimeStatus pendingTransport = new RuntimeStatus { Transport="native-pending" };
                if (pendingTransport.TransportLabel != "GPU 전달 확인 중" || !pendingTransport.TransportDetail.Contains("아직 확인되지")) throw new Exception("Pending native transport must not claim verified GPU delivery.");
                RuntimeStatus systemTransport = new RuntimeStatus { Transport="system-memory" };
                if (!systemTransport.TransportLabel.Contains("RAM") || !systemTransport.TransportDetail.Contains("아직 확인되지")) throw new Exception("System-memory transport must not claim verified native GPU delivery.");
                RuntimeStatus missingTransport = new RuntimeStatus();
                if (missingTransport.GpuCorrection.HasValue || missingTransport.AppearanceProtection.HasValue) throw new Exception("Old telemetry cannot invent enhancement settings.");
                if (missingTransport.TransportLabel != "아직 보고 없음") throw new Exception("Missing transport must not imply a known path.");
                RuntimeStatus unknownTransport = new RuntimeStatus { Transport="d3d11-native" };
                if (!unknownTransport.TransportLabel.StartsWith("확인되지 않은")) throw new Exception("Unknown transport must not be assumed native.");
                Window testWindow;
                using (Stream xaml = Assembly.GetExecutingAssembly().GetManifestResourceStream("MainWindow.xaml")) testWindow = (Window)XamlReader.Load(xaml);
                Controller testController = new Controller(testWindow, true);
                testController.PreviewState("excluded");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "보간 대상에서 제외된 영상") throw new Exception("Excluded-rate UI must be distinct from master-off.");
                testController.PreviewState("above-target");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "출력 FPS 이상 · 원본 재생") throw new Exception("At-target bypass needs a neutral, distinct explanation.");
                testController.PreviewState("unknown-rate");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "원본 FPS 확인 불가 · 원본 재생" || !((TextBlock)testWindow.FindName("StatusDetail")).Text.Contains("×2")) throw new Exception("Unknown source FPS must explain the double-rate bypass without claiming master-off.");
                testController.PreviewState("unsupported-rate");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "지원 범위 밖 · 원본 재생" || !((TextBlock)testWindow.FindName("StatusDetail")).Text.Contains("지원 범위")) throw new Exception("Unsupported double output must explain the original-rate fallback.");
                CheckBox rate24 = (CheckBox)testWindow.FindName("InputRate0");
                CheckBox rate60 = (CheckBox)testWindow.FindName("InputRate4");
                bool? saved24 = rate24.IsChecked, saved60 = rate60.IsChecked;
                testController.PreviewState("disabled");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "프레임 보간 꺼짐" || rate24.IsEnabled || rate60.IsEnabled || rate24.IsChecked != saved24 || rate60.IsChecked != saved60) throw new Exception("Master-off must disable source-rate controls without clearing their selection.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked = true;
                if (!rate24.IsEnabled || !rate60.IsEnabled) throw new Exception("Master-on must restore source-rate controls.");
                ToggleButton gpuToggle=(ToggleButton)testWindow.FindName("GpuCorrectionToggle"), mouthToggle=(ToggleButton)testWindow.FindName("AppearanceProtectionToggle");
                gpuToggle.IsChecked=false; mouthToggle.IsChecked=true;
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked=false;
                if(gpuToggle.IsEnabled||mouthToggle.IsEnabled||gpuToggle.IsChecked!=false||mouthToggle.IsChecked!=true) throw new Exception("Master-off changed enhancement selection.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked=true;
                if(!gpuToggle.IsEnabled||!mouthToggle.IsEnabled||gpuToggle.IsChecked!=false||mouthToggle.IsChecked!=true) throw new Exception("Independent enhancement selection lost.");
                testController.PreviewState("double");
                if (testWindow.FindName("Fps60") != null || testWindow.FindName("Fps120") != null || !((TextBlock)testWindow.FindName("OutputRateSummary")).Text.Contains("×2") || !((TextBlock)testWindow.FindName("OutputRateHint")).Text.Contains("47.952")) throw new Exception("Controller must expose only double-rate output.");
                testController.PreviewState("error");
                if (!((TextBox)testWindow.FindName("DiagnosticsText")).Text.Contains("AnotherLongExampleFolder")) throw new Exception("Full action error details must remain available without expanding the footer.");
                foreach (int width in new int[] { 504, 464 })
                {
                    ((TabItem)testWindow.FindName("InterpolationTab")).IsSelected = true;
                    FrameworkElement client = (FrameworkElement)testWindow.Content;
                    client.Measure(new Size(width, 461)); client.Arrange(new Rect(0, 0, width, 461)); client.UpdateLayout();
                    TextBlock saveText = (TextBlock)testWindow.FindName("SaveNotice");
                    double footerTop = saveText.TransformToAncestor(client).Transform(new Point(0, 0)).Y - 8;
                    foreach (string controlName in new string[] { "EnabledToggle", "InputRate0", "InputRate5", "OutputRateSummary", "OutputRateHint" })
                    {
                        FrameworkElement item = (FrameworkElement)testWindow.FindName(controlName);
                        Rect bounds = item.TransformToAncestor(client).TransformBounds(new Rect(0, 0, item.ActualWidth, item.ActualHeight));
                        if (bounds.Left < 0 || bounds.Right > width || bounds.Top < 0 || bounds.Bottom > footerTop) throw new Exception("Essential control clipped at " + width + ": " + controlName);
                    }
                }
                ((TabItem)testWindow.FindName("EnhancementsTab")).IsSelected=true;
                FrameworkElement optionClient=(FrameworkElement)testWindow.Content;
                optionClient.Measure(new Size(464,461)); optionClient.Arrange(new Rect(0,0,464,461)); optionClient.UpdateLayout();
                double optionFooter=((TextBlock)testWindow.FindName("SaveNotice")).TransformToAncestor(optionClient).Transform(new Point(0,0)).Y-8;
                foreach(string name in new[]{"GpuCorrectionToggle","AppearanceProtectionToggle","EnhancementHint"}) {
                    FrameworkElement item=(FrameworkElement)testWindow.FindName(name);
                    Rect b=item.TransformToAncestor(optionClient).TransformBounds(new Rect(0,0,item.ActualWidth,item.ActualHeight));
                    if(b.Left<0||b.Right>464||b.Bottom>optionFooter)throw new Exception("Enhancement control clipped: "+name);
                }
                ((TabItem)testWindow.FindName("InterpolationTab")).IsSelected=true;
                System.Windows.Input.FocusManager.SetFocusedElement(testWindow, rate24);
                if (!rate24.Focusable || !rate24.IsTabStop || !rate24.IsFocused) throw new Exception("Native checkbox logical focus is unavailable.");
                ((TabItem)testWindow.FindName("SetupTab")).IsSelected = true;
                if (!((TabItem)testWindow.FindName("SetupTab")).IsSelected) throw new Exception("Connection tab is unavailable.");
                ((TabItem)testWindow.FindName("DiagnosticsTab")).IsSelected = true;
                if (!((TabItem)testWindow.FindName("DiagnosticsTab")).IsSelected) throw new Exception("Diagnostics tab is unavailable.");
                testWindow.Close();
                bool? ownElevation = RegistrationInfo.IsElevated(Process.GetCurrentProcess().Id);
                if (!ownElevation.HasValue) throw new Exception("Read-only token elevation query failed.");
                ProcessStartInfo userRegistration = RegistrationInfo.CreateStartInfo(folder, false);
                if (userRegistration.UseShellExecute || userRegistration.Arguments != "--register" || !String.IsNullOrEmpty(userRegistration.Verb)) throw new Exception("Per-user registration must not request elevation.");
                ProcessStartInfo machineRegistration = RegistrationInfo.CreateStartInfo(folder, true);
                if (!machineRegistration.UseShellExecute || machineRegistration.Verb != "runas" || machineRegistration.Arguments != "--register-machine") throw new Exception("Machine registration must use the explicit administrator action.");
                if (machineRegistration.RedirectStandardOutput || machineRegistration.RedirectStandardError) throw new Exception("Elevated shell execution cannot redirect streams.");
                File.WriteAllText(Path.Combine(folder, "passed.txt"), "Settings, status, transport labels, and registration routing tests passed. No registration process was launched.");
                return 0;
            }
            finally { Directory.Delete(folder, true); }
        }
    }
}



