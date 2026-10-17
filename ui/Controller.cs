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
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
        private static extern uint GetPrivateProfileString(string section,string key,string fallback,StringBuilder value,uint size,string file);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WritePrivateProfileString(string section, string key, string value, string file);
        public SettingsStore(string path) { this.path = path; }
        public bool Enabled { get { return GetPrivateProfileInt("Nvof", "Enabled", 1, path) != 0; } }
        public static readonly int[] OutputLimits = {60,72,75,100,120,144,160,240};
        public int OutputFpsLimit { get { int value=(int)GetPrivateProfileInt("Nvof", "OutputFpsLimit", 60, path); return Array.IndexOf(OutputLimits,value)>=0?value:60; } }
        public int OutputMode { get { return GetPrivateProfileInt("Nvof", "OutputMode", 0, path)==1?1:0; } }
        public int OutputMultiplier { get { int value=(int)GetPrivateProfileInt("Nvof", "OutputMultiplier", 2, path); return value>=2&&value<=5?value:2; } }
        public bool ResolutionRulesEnabled {get {return GetPrivateProfileInt("Nvof","ResolutionRulesEnabled",0,path)!=0;}}
        public int[] ResolutionRules {get {var text=new StringBuilder(256);GetPrivateProfileString("Nvof","ResolutionRules","",text,256,path);return ResolutionSettings.Parse(text.ToString());}}
        public void SetResolutionRulesEnabled(bool value) {Write("ResolutionRulesEnabled",value?"1":"0");}
        public void SetResolutionConfiguration(int[] rules,int common)
        {
            if(common==0 || common==-1 || !ResolutionSettings.Valid(common))throw new ArgumentOutOfRangeException("common");
            var changes=new Dictionary<string,string> {
                {"ResolutionRules",ResolutionSettings.Serialize(rules)}, {"ResolutionRulesEnabled","1"},
                {"OutputMode",common<0?"1":"0"},
                {common<0?"OutputMultiplier":"OutputFpsLimit",Math.Abs(common).ToString(CultureInfo.InvariantCulture)}
            };
            // Publish the whole draft together. An open editor, Cancel and a
            // failed save never expose half of a resolution table to playback.
            string temporary=path+"."+Guid.NewGuid().ToString("N")+".tmp";
            try {
                if(File.Exists(path))File.Copy(path,temporary);
                foreach(var change in changes)
                    if(!WritePrivateProfileString("Nvof",change.Key,change.Value,temporary))
                        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                WritePrivateProfileString(null,null,null,temporary);
                if(File.Exists(path))File.Replace(temporary,path,null);else File.Move(temporary,path);
            } finally {if(File.Exists(temporary))File.Delete(temporary);}
        }
        public void SetOutputMode(int value) { if(value!=0&&value!=1)throw new ArgumentOutOfRangeException("value"); Write("OutputMode",value.ToString(CultureInfo.InvariantCulture)); }
        public void SetOutputMultiplier(int value) { if(value<2||value>5)throw new ArgumentOutOfRangeException("value"); Write("OutputMultiplier",value.ToString(CultureInfo.InvariantCulture)); }
        public void SetOutputFpsLimit(int value)
        {
            if (Array.IndexOf(OutputLimits,value)<0) throw new ArgumentOutOfRangeException("value");
            Write("OutputFpsLimit", value.ToString(CultureInfo.InvariantCulture));
        }
        public int InputHeightLimit { get { uint value=GetPrivateProfileInt("Nvof", "InputHeightLimit", 0, path); return value==1080 || value==1440 ? (int)value : 0; } }
        public void SetInputHeightLimit(int value)
        {
            if(value!=0 && value!=1080 && value!=1440)throw new ArgumentOutOfRangeException("value");
            Write("InputHeightLimit", value.ToString(CultureInfo.InvariantCulture));
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
        public string InputFormat="", OutputFormat="", ColorMode="";
        public string BypassReason = "";
        public bool InputRateSelected = true;
        public int InputRateMask = 63;
        public bool DoubleRate;
        public int OutputFpsLimit, OutputMultiple, OutputMode, OutputMultiplier;
        public int InputHeightLimit, InputHeight;
        public bool ResolutionRulesEnabled;
        public int ResolutionBand, ResolutionRule;
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
                status.InputFormat=Value<string>(data,"inputSurfaceFormat","");
                status.OutputFormat=Value<string>(data,"outputSurfaceFormat","");
                status.ColorMode=Value<string>(data,"colorMode","");
                status.BypassReason = Value<string>(data, "bypassReason", "");
                status.InputRateSelected = Value<bool>(data, "inputRateSelected", true);
                status.InputHeightLimit=Value<int>(data,"inputHeightLimit",0);
                status.InputHeight=Value<int>(data,"inputHeight",0);
                status.InputRateMask = Value<int>(data, "inputRateMask", 63);
                status.DoubleRate = Value<bool>(data, "doubleRate", false);
                status.OutputFpsLimit = Value<int>(data, "outputFpsLimit", 0);
                status.OutputMultiple = Value<int>(data, "outputMultiple", 0);
                status.OutputMode = Value<int>(data, "outputMode", 0);
                status.OutputMultiplier = Value<int>(data, "outputMultiplier", 2);
                status.ResolutionRulesEnabled=Value<bool>(data,"resolutionRulesEnabled",false);
                status.ResolutionBand=Value<int>(data,"resolutionBand",0);
                status.ResolutionRule=Value<int>(data,"resolutionRule",0);
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
        public bool UserOwned, MachineOwned;
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
                        bool owned = classes != null && (Matches(classes, FilterClsid, expected) || Matches(classes, PageClsid, expected));
                        if (hive == RegistryHive.CurrentUser) { info.UserRegistered = registered; info.UserOwned=owned; }
                        else { info.MachineRegistered = registered; info.MachineOwned=owned; }
                    }
                }
                catch (Exception ex) { info.RegistrationReadError = ex.Message; }
            }
            return info;
        }
        public static ProcessStartInfo CreateStartInfo(string baseDirectory, bool machine, bool remove = false)
        {
            ProcessStartInfo start = new ProcessStartInfo(Path.Combine(baseDirectory, "NvofRegister.exe"), remove ? (machine ? "--unregister-machine" : "--unregister") : (machine ? "--register-machine" : "--register"));
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
        private ToggleButton enabled;
        private readonly CheckBox[] inputRates = new CheckBox[6];
        private TextBlock rateSelectionHint, outputRateHint;
        private RadioButton heightAll, height1080, height1440;
        private ComboBox outputMode, outputValue;
        private int selectedLimit=60, selectedMultiplier=2;
        private int commonOutputMode;
        private int[] resolutionRules=new int[7];
        private bool updatingOutput;
        private TextBlock notice, statusTitle, statusDetail, registerNotice;
        private System.Windows.Shapes.Ellipse statusDot;
        private TextBox diagnosticsText;
        private Button registerButton, unregisterButton;
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
            outputRateHint = Find<TextBlock>("OutputRateHint");
            outputMode = Find<ComboBox>("OutputModeChoice"); outputValue = Find<ComboBox>("OutputValueChoice");
            Find<Button>("ResolutionEditButton").Click+=delegate {
                var editor=CreateResolutionEditor();editor.Window.Owner=window;editor.Window.ShowDialog();
                Find<Button>("ResolutionEditButton").Focus();
            };
            heightAll=Find<RadioButton>("HeightAll");height1080=Find<RadioButton>("Height1080");height1440=Find<RadioButton>("Height1440");
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
            unregisterButton = Find<Button>("UnregisterButton");
            unregisterButton.Click += delegate { UnregisterFilter(); };
            ConfigurePackageUi(FindUninstaller(baseDirectory) != null);
            LoadSettings();
            Find<Button>("InstallFolderButton").Click += delegate {
                try { Process.Start(new ProcessStartInfo(baseDirectory) { UseShellExecute=true }); }
                catch (Exception ex) { Notify("현재 폴더를 열지 못했습니다.", true, ex.Message); }
            };
            Find<Button>("UninstallButton").Click += delegate {
                if (preview) return;
                string uninstaller = FindUninstaller(baseDirectory);
                if (!File.Exists(uninstaller)) { Notify("설치 프로그램으로 설치한 버전에서 제거할 수 있습니다.", true); return; }
                try { Process.Start(new ProcessStartInfo(uninstaller) { UseShellExecute=true, WorkingDirectory=baseDirectory }); window.Close(); }
                catch (Exception ex) { Notify("제거 프로그램을 열지 못했습니다.", true, ex.Message); }
            };
            heightAll.Checked += delegate { SaveHeightLimit(); };
            height1080.Checked += delegate { SaveHeightLimit(); };
            height1440.Checked += delegate { SaveHeightLimit(); };
            outputMode.SelectionChanged += delegate { if(loading || updatingOutput)return; RefreshOutputValues(); SaveOutputChoice(true); };
            outputValue.SelectionChanged += delegate { if(loading || updatingOutput)return; SaveOutputChoice(false); };
            enabled.Click += delegate { SaveEnabled(); };
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
            selectedLimit=settings.OutputFpsLimit; selectedMultiplier=settings.OutputMultiplier;
            commonOutputMode=settings.OutputMode;resolutionRules=settings.ResolutionRules;
            outputMode.SelectedIndex=settings.ResolutionRulesEnabled?2:commonOutputMode;
            RefreshOutputValues();
            heightAll.IsChecked=settings.InputHeightLimit==0;
            height1080.IsChecked=settings.InputHeightLimit==1080;
            height1440.IsChecked=settings.InputHeightLimit==1440;
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
                : "23.976 → 24 · 29.97 → 30 · 59.94 → 60에 포함";
        }
        private void SaveInputRates()
        {
            UpdateRateHint();
            if (loading || preview) return;
            try { settings.SetInputRateMask(SelectedRateMask()); Notify("저장됨 · 영상을 다시 열면 적용됩니다.", false); }
            catch (Exception ex) { LoadSettings(); Notify("설정을 저장하지 못했습니다. 폴더 쓰기 권한을 확인하세요.", true, ex.Message); }
        }
        private void RefreshOutputValues()
        {
            updatingOutput=true;
            bool custom=outputMode.SelectedIndex==2;
            bool multiple=outputMode.SelectedIndex==1 || (custom && commonOutputMode==1);
            outputValue.Visibility=custom?Visibility.Collapsed:Visibility.Visible;
            Find<Button>("ResolutionEditButton").Visibility=custom?Visibility.Visible:Visibility.Collapsed;
            outputValue.Items.Clear();
            foreach(int value in multiple ? new int[] {2,3,4,5} : SettingsStore.OutputLimits)
                outputValue.Items.Add(new ComboBoxItem { Content=multiple?"×"+value:"최대 "+value+" fps", Tag=value });
            int selected=multiple?selectedMultiplier:selectedLimit;
            foreach(ComboBoxItem item in outputValue.Items) if((int)item.Tag==selected) outputValue.SelectedItem=item;
            Find<TextBlock>("OutputValueLabel").Text=custom?"사용자 설정":multiple?"배수":"FPS 상한";
            updatingOutput=false;
            UpdateOutputHint();
        }
        private void UpdateOutputHint()
        {
            if(outputMode.SelectedIndex==2)
                outputRateHint.Text="세로 높이에 맞는 출력 설정을 적용합니다.\n기본 출력: "+ResolutionSettings.Label(commonOutputMode==1?-selectedMultiplier:selectedLimit);
            else if(outputMode.SelectedIndex==1)
                outputRateHint.Text="24 → "+(24*selectedMultiplier)+" fps · 30 → "+(30*selectedMultiplier)+" fps\n선택한 배수로 보간하며, 원본 프레임을 유지합니다.";
            else {
                int m24=Math.Max(1,selectedLimit/24),m25=Math.Max(1,selectedLimit/25);
                outputRateHint.Text="24 → "+(24*m24)+" fps · 25 → "+(25*m25)+" fps\n상한 안에서 가능한 가장 큰 정수배로 보간합니다.";
            }
        }
        private string OutputChoiceLabel() { return outputMode.SelectedIndex==2?"해상도별 설정":outputMode.SelectedIndex==1?"원본 배수 ×"+selectedMultiplier:"최대 "+selectedLimit+" fps · 정수배"; }
        public ResolutionEditor CreateResolutionEditor(bool fixture=false)
        {
            int[] rules=fixture?new int[]{0,120,120,120,-3,-2,-1}:(int[])resolutionRules.Clone();
            var editor=new ResolutionEditor(window,rules,commonOutputMode==1?-selectedMultiplier:selectedLimit,
                SelectedHeightLimit(),SelectedRateMask(),delegate(int[] values,int common) {
                    if(!preview)settings.SetResolutionConfiguration(values,common);
                    resolutionRules=(int[])values.Clone();commonOutputMode=common<0?1:0;
                    if(common<0)selectedMultiplier=-common;else selectedLimit=common;
                    loading=true;outputMode.SelectedIndex=2;RefreshOutputValues();loading=false;
                    Notify("해상도별 설정 저장됨 · 영상을 다시 열면 적용됩니다.",false);
                });
            if(fixture)((TextBlock)editor.Window.FindName("ResolutionSaveNotice")).Text="미리보기 · 설정 예시 (실제 저장값이 아닙니다)";
            return editor;
        }
        private int SelectedHeightLimit() { return height1080.IsChecked==true?1080:height1440.IsChecked==true?1440:0; }
        private static string HeightLabel(int value) { return value==0?"전체":value+"p 이하"; }
        private void SaveHeightLimit()
        {
            if(loading || preview)return;
            try { settings.SetInputHeightLimit(SelectedHeightLimit()); Notify("저장됨 · 영상을 다시 열면 적용됩니다.", false); }
            catch(Exception ex) { LoadSettings(); Notify("설정을 저장하지 못했습니다. 폴더 쓰기 권한을 확인하세요.", true, ex.Message); }
        }
        private void SaveOutputChoice(bool modeChanged)
        {
            ComboBoxItem item=outputValue.SelectedItem as ComboBoxItem;
            if(item==null)return;
            if(outputMode.SelectedIndex<2) {
                commonOutputMode=outputMode.SelectedIndex;
                if(commonOutputMode==1)selectedMultiplier=(int)item.Tag;else selectedLimit=(int)item.Tag;
            }
            UpdateOutputHint();
            if(loading || preview)return;
            try {
                if(modeChanged) {
                    if(outputMode.SelectedIndex<2)settings.SetOutputMode(outputMode.SelectedIndex);
                    settings.SetResolutionRulesEnabled(outputMode.SelectedIndex==2);
                }
                else if(outputMode.SelectedIndex==1)settings.SetOutputMultiplier(selectedMultiplier);
                else settings.SetOutputFpsLimit(selectedLimit);
                Notify("저장됨 · 영상을 다시 열면 적용됩니다.",false);
            } catch(Exception ex) { LoadSettings(); Notify("설정을 저장하지 못했습니다. 폴더 쓰기 권한을 확인하세요.",true,ex.Message); }
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
                if (status.BypassReason == "resolution-rule")
                {
                    statusTitle.Text=recent?"해상도별 설정 · 원본 재생":"마지막 재생 상태 · 해상도별 보간 꺼짐";
                    statusDetail.Text=status.InputHeight+"p · "+Fps(status.InputFps)+" fps · 이 구간은 ‘보간 안 함’으로 설정되어 있습니다.";
                }
                else if (status.BypassReason == "source-height")
                {
                    statusTitle.Text=recent?"해상도 조건으로 원본 재생":"마지막 재생 상태 · 해상도 조건으로 제외";
                    statusDetail.Text=status.InputHeight+"p · "+Fps(status.OutputFps)+" fps · 적용 기준: "+HeightLabel(status.InputHeightLimit);
                }
                else if (status.BypassReason == "source-rate")
                {
                    statusTitle.Text = recent ? "보간 대상에서 제외된 영상" : "마지막 재생 상태 · 보간 대상에서 제외";
                    statusDetail.Text = Fps(status.InputFps) + " fps 원본으로 재생" + (recent ? " 중입니다. 보간할 원본 프레임 선택에서 제외되어 있습니다." : "했습니다. 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "at-or-above-target")
                {
                    statusTitle.Text = recent ? "상한 내 정수배 없음 · 원본 재생" : "마지막 재생 상태 · 원본 재생";
                    statusDetail.Text = Fps(status.InputFps) + " fps 영상은 선택한 상한 안에서 두 배로 늘릴 수 없어 원본으로 재생합니다." + (recent ? "" : " 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "unknown-source-rate")
                {
                    statusTitle.Text = recent ? "원본 FPS 확인 불가 · 원본 재생" : "마지막 재생 상태 · 원본 FPS 확인 불가";
                    statusDetail.Text = "원본 영상의 FPS를 확인할 수 없어 정수배 보간을 적용하지 않습니다." + (recent ? "" : " 현재 재생 상태는 업데이트 대기 중입니다.");
                }
                else if (status.BypassReason == "output-rate-unsupported")
                {
                    statusTitle.Text = recent ? "지원 범위 밖 · 원본 재생" : "마지막 재생 상태 · 지원 범위 밖";
                    statusDetail.Text = "출력 FPS가 지원 범위를 벗어나 원본 프레임을 유지합니다." + (recent ? "" : " 현재 재생 상태는 업데이트 대기 중입니다.");
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
            text.AppendLine("NVOF for PotPlayer " + (Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0] ?? "버전 확인 불가"));
            if (registration != null)
            {
                text.AppendLine("팟플레이어 권한: " + registration.PlayerDescription);
                text.AppendLine("사용자별 필터·설정 창 등록: " + (registration.UserRegistered ? "이 폴더 등록됨" : "없음 또는 다른 폴더"));
                text.AppendLine("관리자용 필터·설정 창 등록: " + (registration.MachineRegistered ? "이 폴더 등록됨" : "없음 또는 다른 폴더"));
                text.AppendLine("현재 등록 버튼: " + (registration.ElevatedPlayer ? "관리자용 (Windows 승인 필요)" : "현재 사용자용"));
                if (registration.ElevatedPlayer && !registration.MachineRegistered) text.AppendLine("관리자 팟플레이어에서는 사용자별 COM 등록을 사용하지 않습니다. 관리자용 등록이 필요합니다.");
                if (!String.IsNullOrEmpty(registration.RegistrationReadError)) text.AppendLine("등록 조회 오류: " + registration.RegistrationReadError);
            }
            text.AppendLine("설정: " + (enabled.IsChecked == true ? "사용" : "사용 안 함") + " / " + OutputChoiceLabel());
            string[] rateLabels = { "24", "25", "30", "50", "60", "기타" };
            List<string> selectedLabels = new List<string>();
            for (int index = 0; index < inputRates.Length; index++)
                if (inputRates[index].IsChecked == true) selectedLabels.Add(rateLabels[index]);
            text.AppendLine("보간할 원본 프레임: " + (selectedLabels.Count == 0 ? "선택 없음" : String.Join(", ", selectedLabels.ToArray()) + " fps"));
            text.AppendLine("보간할 세로 해상도: "+HeightLabel(SelectedHeightLimit()));
            if(outputMode.SelectedIndex==2)for(int i=0;i<7;++i)
                text.AppendLine(ResolutionSettings.BandLabel(i)+": "+ResolutionSettings.Label(resolutionRules[i]));
            text.AppendLine("영상 전달: " + status.TransportLabel);
            if(!String.IsNullOrEmpty(status.InputFormat))text.AppendLine("입력 형식: "+status.InputFormat+(status.InputFormat=="P010"?" · 10bit":status.InputFormat=="NV12"?" · 8bit":""));
            if(!String.IsNullOrEmpty(status.OutputFormat))text.AppendLine("출력 형식: "+status.OutputFormat+(status.OutputFormat=="P010"?" · 10bit":" · 8bit"));
            if(!String.IsNullOrEmpty(status.ColorMode))text.AppendLine("영상 색 정보: "+status.ColorMode+" (디스플레이 HDR 활성화 여부와는 별개)");
            text.AppendLine("필터: " + (File.Exists(Path.Combine(baseDirectory, "NvofPotPlayer.ax")) ? "설치 파일 있음" : "설치 파일 없음"));
            text.AppendLine("상태: " + (status.Exists ? status.State : "아직 보고 없음"));
            if (status.Exists)
            {
                text.AppendLine("프로세스: " + status.ProcessId + (status.ProcessAlive ? " (실행 중)" : " (종료됨)"));
                text.AppendLine("FPS: " + Fps(status.InputFps) + " → " + Fps(status.OutputFps));
                text.AppendLine("출력 프레임: " + status.OutputFrames);
                text.AppendLine("재생에 적용된 세로 해상도: "+HeightLabel(status.InputHeightLimit)+" · 입력 "+status.InputHeight+"p");
                text.AppendLine("재생에 적용된 원본 프레임 선택: " + status.InputRateMask);
                if(status.ResolutionRulesEnabled)text.AppendLine("재생에 적용된 해상도별 설정: "+(status.ResolutionBand==0?"480p 미만":status.ResolutionBand+"p 이상")+" · "+ResolutionSettings.Label(status.ResolutionRule));
                text.AppendLine("재생에 적용된 출력 모드: " + (status.OutputMode==1 ? "원본 배수 ×"+status.OutputMultiplier+" · 실제 ×"+status.OutputMultiple : status.OutputFpsLimit > 0 ? "최대 " + status.OutputFpsLimit + " fps · 실제 ×" + status.OutputMultiple : status.DoubleRate ? "원본의 두 배 (×2)" : "이전 필터 · 보고 없음"));
                if (!String.IsNullOrEmpty(status.BypassReason)) text.AppendLine("원본 재생 이유: " + (status.BypassReason == "source-height" ? "세로 해상도 조건에서 제외" : status.BypassReason == "source-rate" ? "원본 프레임 선택에서 제외" : status.BypassReason == "disabled" ? "보간 사용 꺼짐" : status.BypassReason == "at-or-above-target" ? "상한 내 정수배 없음" : status.BypassReason == "unknown-source-rate" ? "원본 FPS 확인 불가" : status.BypassReason == "output-rate-unsupported" ? "출력 FPS가 지원 범위 밖" : status.BypassReason));
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
            unregisterButton.IsEnabled = filesExist && !registering && registration != null && (registration.UserOwned || registration.MachineOwned);
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
        internal void ConfigurePackageUi(bool installed)
        {
            Find<Button>("UninstallButton").Visibility = installed ? Visibility.Visible : Visibility.Collapsed;
            Find<TextBlock>("PackageHint").Text = installed
                ? "제거 전 팟플레이어를 닫아 주세요. 관리자용 등록을 해제할 때는 Windows 승인이 필요합니다."
                : "폴더를 옮기거나 삭제하기 전, 팟플레이어를 닫고 등록을 해제하세요. 옮긴 폴더에서는 다시 등록하면 됩니다.";
        }
        private async void UnregisterFilter()
        {
            if (preview || registering) return;
            registration = RegistrationInfo.Read(baseDirectory);
            if (registration.PlayerRunning) { Notify("팟플레이어를 완전히 종료한 뒤 등록을 해제하세요.", true); return; }
            if (!String.IsNullOrEmpty(registration.RegistrationReadError)) { Notify("등록 상태를 확인하지 못했습니다. 진단 정보를 확인하세요.", true, registration.RegistrationReadError); return; }
            bool machine=registration.MachineOwned, user=registration.UserOwned;
            registering=true; registrationResult=null; registrationResultError=false;
            RefreshRegistrationNotice();
            registerNotice.Text=machine ? "관리자용 등록 해제를 위해 Windows 승인을 요청합니다…" : "필터 등록을 해제하는 중입니다…";
            try
            {
                await Task.Run(delegate {
                    // Remove the machine entry first: cancellation leaves the user's registration intact.
                    foreach (bool elevated in new[] { true, false })
                    {
                        if (elevated ? !machine : !user) continue;
                        using (Process process=Process.Start(RegistrationInfo.CreateStartInfo(baseDirectory,elevated,true)))
                        {
                            if(process==null)throw new IOException("등록 해제 도구를 시작하지 못했습니다.");
                            Task<string> output=elevated?null:process.StandardOutput.ReadToEndAsync();
                            Task<string> error=elevated?null:process.StandardError.ReadToEndAsync();
                            if(!process.WaitForExit(30000))throw new TimeoutException("해제 작업이 아직 끝나지 않았습니다. 폴더를 이동하거나 삭제하지 마세요.");
                            if(!elevated)Task.WaitAll(output,error);
                            if(process.ExitCode!=0)throw new IOException("등록 해제 도구 종료 코드: "+process.ExitCode+(elevated?"":" · "+error.Result));
                        }
                    }
                    RegistrationInfo verified=RegistrationInfo.Read(baseDirectory);
                    if(verified.UserOwned || verified.MachineOwned || !String.IsNullOrEmpty(verified.RegistrationReadError))
                        throw new IOException("등록 해제 결과를 확인하지 못했습니다. 폴더를 그대로 두고 진단 정보를 확인하세요.");
                });
                registrationResult="이 폴더의 필터 등록을 해제했습니다. 설정 창을 닫은 뒤 폴더를 이동하거나 삭제할 수 있습니다.";
            }
            catch (System.ComponentModel.Win32Exception ex) {
                registrationResultError=true;
                registrationResult=ex.NativeErrorCode==1223 ? "Windows 승인이 취소되어 등록을 해제하지 않았습니다." : "등록 해제 실패: "+ex.Message;
            }
            catch(Exception ex) { registrationResultError=true;registrationResult="등록 해제를 완료하지 못했습니다. "+ex.Message; }
            finally { registering=false;RefreshStatus(); }
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
            if(state=="limit120") { fixture.State="active";fixture.OutputFps=120000.0/1001;fixture.OutputFpsLimit=120;fixture.OutputMultiple=5;fixture.Transport="d3d11-gpu";selectedLimit=120;outputMode.SelectedIndex=0;RefreshOutputValues(); }
            if(state=="limit240") { selectedLimit=240;outputMode.SelectedIndex=0;RefreshOutputValues(); fixture.State="active";fixture.OutputFps=239.760;fixture.OutputFpsLimit=240;fixture.OutputMultiple=10; }
            if(state=="multiple5") { selectedMultiplier=5;outputMode.SelectedIndex=1;RefreshOutputValues(); fixture.State="active";fixture.OutputFps=119.880;fixture.OutputMode=1;fixture.OutputMultiplier=5;fixture.OutputMultiple=5; }
            if(state=="resolution" || state=="resolution-editor" || state=="resolution-disabled") {
                selectedLimit=120;commonOutputMode=0;outputMode.SelectedIndex=2;RefreshOutputValues();
                fixture.State="active";fixture.Transport="d3d11-gpu";fixture.OutputFps=119.880;
                fixture.ResolutionRulesEnabled=true;fixture.ResolutionBand=1080;fixture.ResolutionRule=120;
                if(state=="resolution-disabled")enabled.IsChecked=false;
            }
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
            if (state == "rates-mixed" || state == "rates-none")
            {
                for(int i=0;i<inputRates.Length;++i)inputRates[i].IsChecked=state=="rates-mixed" && (i==0 || i==2 || i==4);
                UpdateRateHint();
                fixture.State=state=="rates-none"?"bypass":"active";fixture.BypassReason="source-rate";
                fixture.Transport="d3d11-gpu";fixture.InputRateMask=SelectedRateMask();
                if(state=="rates-none")fixture.OutputFps=fixture.InputFps;
            }
            ShowStatus(fixture);
            notice.Text = "미리보기 · 상태 예시 (실제 실행 기록이 아닙니다)";
            if (state == "error")
            {
                Notify("미리보기 · 저장 실패. 폴더 쓰기 권한을 확인하세요.", true,
                    @"Preview fixture: Access to a deliberately long example path F:\ExampleFolder\AnotherLongExampleFolder\NvofPotPlayer\NvofPotPlayer.ini was denied. Additional diagnostic details remain available in the diagnostics tab.");
                ShowStatus(fixture);
            }
            if (state == "portable" || state == "installed") { ConfigurePackageUi(state=="installed"); registerNotice.Text="현재 폴더의 필터가 등록돼 있습니다."; unregisterButton.IsEnabled=true; Find<TabItem>("SetupTab").IsSelected=true; }
            if (state == "setup") Find<TabItem>("SetupTab").IsSelected = true;
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
                    int height = Int32.Parse(Arg(args, "--preview-height", "501"), CultureInfo.InvariantCulture);
                    if (width < 360 || width > 3000 || height < 300 || height > 3000) throw new ArgumentOutOfRangeException("preview size");
                    controller.PreviewState(state);
                    if(state=="resolution-editor")window=controller.CreateResolutionEditor(true).Window;
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
                    string menuName = Arg(args, "--preview-menu", "");
                    if (!String.IsNullOrEmpty(menuName))
                    {
                        ComboBox combo = window.FindName(menuName) as ComboBox;
                        if (combo == null) throw new ArgumentException("Unknown menu preview target.");
                        combo.ApplyTemplate();
                        Popup popup = combo.Template.FindName("PART_Popup", combo) as Popup;
                        if (popup == null || popup.Child == null) throw new InvalidOperationException("Select menu template is missing.");
                        // Render the actual template tree without opening a window or altering settings.
                        content = (FrameworkElement)popup.Child;
                        width = (int)Math.Ceiling(combo.ActualWidth);
                        content.Measure(new Size(width, Double.PositiveInfinity));
                        height = (int)Math.Ceiling(content.DesiredSize.Height);
                        content.Arrange(new Rect(0, 0, width, height));
                        content.UpdateLayout();
                    }
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
                if(store.InputHeightLimit!=0)throw new Exception("Default height limit must include all videos.");
                foreach(int height in new int[] {1080,1440,0}) {
                    store.SetInputHeightLimit(height);store.SetEnabled(false);
                    if(new SettingsStore(settingsPath).InputHeightLimit!=height || store.Enabled)throw new Exception("Height limit persistence failed.");
                }
                bool invalidHeight=false;try {store.SetInputHeightLimit(2160);}catch(ArgumentOutOfRangeException){invalidHeight=true;}
                if(!invalidHeight)throw new Exception("Unsupported height limit accepted.");
                if (store.InputRateMask != 63) throw new Exception("Missing source-rate selection must preserve all-rates behavior.");
                if (!store.GpuCorrection || !store.AppearanceProtection) throw new Exception("Existing INIs must retain both enhancements by default.");
                foreach (bool gpu in new bool[] {false,true}) foreach (bool mouth in new bool[] {false,true})
                {
                    store.SetGpuCorrection(gpu); store.SetAppearanceProtection(mouth); store.SetEnabled(false); store.SetOutputFpsLimit(120);
                    if (store.GpuCorrection != gpu || store.AppearanceProtection != mouth) throw new Exception("Independent enhancement settings were coupled or migration reset them.");
                }
                foreach (int limit in SettingsStore.OutputLimits)
                {
                    store.SetOutputFpsLimit(limit);
                    if (new SettingsStore(settingsPath).OutputFpsLimit != limit || store.Enabled) throw new Exception("Output limit persistence reset another setting.");
                }
                File.WriteAllText(settingsPath,"[Nvof]\r\nTargetFps=120\r\nDoubleRate=0\r\nOutputFpsLimit=119\r\n");
                if(store.OutputFpsLimit!=60)throw new Exception("Invalid/legacy output settings must default to capped 60.");
                bool invalidLimitRejected=false;
                try { store.SetOutputFpsLimit(119); } catch(ArgumentOutOfRangeException) { invalidLimitRejected=true; }
                if(!invalidLimitRejected)throw new Exception("Invalid output limit accepted.");
                if(store.OutputMode!=0 || store.OutputMultiplier!=2)throw new Exception("Old INI must retain cap mode and default x2.");
                foreach(int multiple in new int[] {2,3,4,5}) {
                    store.SetOutputFpsLimit(144);store.SetOutputMultiplier(multiple);store.SetOutputMode(1);
                    if(new SettingsStore(settingsPath).OutputMultiplier!=multiple || store.OutputMode!=1 || store.OutputFpsLimit!=144)throw new Exception("Direct multiplier persistence failed.");
                    store.SetOutputMode(0); if(store.OutputMultiplier!=multiple || store.OutputFpsLimit!=144)throw new Exception("Mode switch forgot independent choice.");
                }
                bool badMode=false,badMultiple=false;
                try{store.SetOutputMode(2);}catch(ArgumentOutOfRangeException){badMode=true;}
                try{store.SetOutputMultiplier(6);}catch(ArgumentOutOfRangeException){badMultiple=true;}
                if(!badMode||!badMultiple)throw new Exception("Unsupported output choice accepted.");
                if(store.ResolutionRulesEnabled || ResolutionSettings.Serialize(store.ResolutionRules)!="0,0,0,0,0,0,0")throw new Exception("Legacy INI changed resolution behavior.");
                File.AppendAllText(settingsPath,"\r\nKeepThis=42\r\n[Unrelated]\r\nValue=retained\r\n");
                var customRules=new int[]{0,240,144,120,-3,-2,-1};
                store.SetResolutionConfiguration(customRules,120);
                if(!store.ResolutionRulesEnabled || store.OutputMode!=0 || store.OutputFpsLimit!=120 ||
                    ResolutionSettings.Serialize(store.ResolutionRules)!="0,240,144,120,-3,-2,-1" || !File.ReadAllText(settingsPath).Contains("Value=retained"))throw new Exception("Atomic resolution save or unrelated settings preservation failed.");
                store.SetResolutionConfiguration(customRules,-3);
                if(store.OutputMode!=1 || store.OutputMultiplier!=3 || store.OutputFpsLimit!=120)throw new Exception("Common selection lost remembered cap.");
                store.SetResolutionRulesEnabled(false);
                if(store.ResolutionRulesEnabled || store.ResolutionRules[6]!=-1)throw new Exception("Turning custom rules off lost choices.");
                string beforeInvalid=File.ReadAllText(settingsPath);bool badRule=false;
                try{store.SetResolutionConfiguration(new int[]{0,0,0,0,0,0,119},120);}catch(ArgumentOutOfRangeException){badRule=true;}
                if(!badRule || File.ReadAllText(settingsPath)!=beforeInvalid)throw new Exception("Invalid rules partially changed settings.");
                foreach(string invalid in new[]{"","0,120","0,0,0,0,0,0,0,0","0,0,0,0,0,0,119","0,0,0,0,0,0,+120","0,0,0,0,0,0,120junk"})
                    if(ResolutionSettings.Serialize(ResolutionSettings.Parse(invalid))!="0,0,0,0,0,0,0")throw new Exception("Invalid resolution syntax accepted.");
                var cap160Rules=new int[]{0,0,0,160,0,120,-4};
                store.SetResolutionConfiguration(cap160Rules,160);
                var cap160Reload=new SettingsStore(settingsPath);
                if(cap160Reload.OutputFpsLimit!=160 || cap160Reload.ResolutionRules[3]!=160 ||
                    cap160Reload.ResolutionRules[5]!=120 || cap160Reload.ResolutionRules[6]!=-4 ||
                    !File.ReadAllText(settingsPath).Contains("Value=retained"))throw new Exception("160 fps cap/rule persistence lost settings.");
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
                File.WriteAllText(statusPath,"{\"state\":\"active\",\"outputMode\":1,\"outputMultiplier\":3,\"outputMultiple\":3,\"outputFpsLimit\":120}");
                RuntimeStatus direct=RuntimeStatus.Read(statusPath);
                if(direct.OutputMode!=1 || direct.OutputMultiplier!=3 || direct.OutputMultiple!=3 || direct.OutputFpsLimit!=120)throw new Exception("Direct-mode telemetry must stay independent of remembered cap.");
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
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "상한 내 정수배 없음 · 원본 재생") throw new Exception("At-target bypass needs a neutral, distinct explanation.");
                testController.PreviewState("unknown-rate");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "원본 FPS 확인 불가 · 원본 재생" || !((TextBlock)testWindow.FindName("StatusDetail")).Text.Contains("정수배")) throw new Exception("Unknown source FPS must explain the double-rate bypass without claiming master-off.");
                testController.PreviewState("unsupported-rate");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "지원 범위 밖 · 원본 재생" || !((TextBlock)testWindow.FindName("StatusDetail")).Text.Contains("지원 범위")) throw new Exception("Unsupported double output must explain the original-rate fallback.");
                CheckBox rate24 = (CheckBox)testWindow.FindName("InputRate0");
                CheckBox rate60 = (CheckBox)testWindow.FindName("InputRate4");
                bool? saved24 = rate24.IsChecked, saved60 = rate60.IsChecked;
                testController.PreviewState("disabled");
                if (((TextBlock)testWindow.FindName("StatusTitle")).Text != "프레임 보간 꺼짐" || rate24.IsEnabled || rate60.IsEnabled || rate24.IsChecked != saved24 || rate60.IsChecked != saved60) throw new Exception("Master-off must disable source-rate controls without clearing their selection.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked = true;
                if (!rate24.IsEnabled || !rate60.IsEnabled) throw new Exception("Master-on must restore source-rate controls.");
                testController.PreviewState("double");
                ComboBox mode=(ComboBox)testWindow.FindName("OutputModeChoice"), value=(ComboBox)testWindow.FindName("OutputValueChoice");
                mode.SelectedIndex=0;
                for(int i=0;i<SettingsStore.OutputLimits.Length;++i) {
                    value.SelectedIndex=i;
                    if((int)((ComboBoxItem)value.SelectedItem).Tag!=SettingsStore.OutputLimits[i])throw new Exception("Output cap list mismatch.");
                }
                mode.SelectedIndex=1;value.SelectedIndex=3;
                if(value.Items.Count!=4 || !((TextBlock)testWindow.FindName("OutputRateHint")).Text.Contains("24 → 120"))throw new Exception("x5 selection or hint failed.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked=false;
                if(mode.IsEnabled || value.IsEnabled || value.SelectedIndex!=3)throw new Exception("Master-off lost output selection.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked=true;
                mode.SelectedIndex=0;
                if((int)((ComboBoxItem)value.SelectedItem).Tag!=240)throw new Exception("Mode switch forgot last cap.");
                value.SelectedIndex=0;
                if(!((TextBlock)testWindow.FindName("OutputRateHint")).Text.Contains("24 → 48"))throw new Exception("Cap mode must not imply exact 60 fps.");
                mode.SelectedIndex=1;
                if((int)((ComboBoxItem)value.SelectedItem).Tag!=5)throw new Exception("Mode switch forgot last multiplier.");
                mode.SelectedIndex=2;
                var editButton=(Button)testWindow.FindName("ResolutionEditButton");
                if(value.Visibility!=Visibility.Collapsed || editButton.Visibility!=Visibility.Visible)throw new Exception("Custom editor entry missing.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked=false;
                if(editButton.IsEnabled)throw new Exception("Master-off left custom controls active.");
                ((ToggleButton)testWindow.FindName("EnabledToggle")).IsChecked=true;
                int draftSaves=0;
                var draft=new ResolutionEditor(testWindow,customRules,120,1080,63,delegate(int[] rules,int common){++draftSaves;store.SetResolutionConfiguration(rules,common);});
                string beforeDraft=File.ReadAllText(settingsPath);
                draft.Choices[3].SelectedIndex=0;
                if(draftSaves!=0 || File.ReadAllText(settingsPath)!=beforeDraft || customRules[3]!=120)throw new Exception("Draft edits leaked before Save.");
                ((Button)draft.Window.FindName("ResolutionResetButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                if(ResolutionSettings.Serialize(draft.Rules)!="0,0,0,0,0,0,0" || draftSaves!=0)throw new Exception("Reset must affect only the draft.");
                ((Button)draft.Window.FindName("ResolutionCancelButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                if(draftSaves!=0 || File.ReadAllText(settingsPath)!=beforeDraft)throw new Exception("Cancel wrote settings.");
                var savedEditor=new ResolutionEditor(testWindow,customRules,-2,0,63,delegate(int[] rules,int common){++draftSaves;store.SetResolutionConfiguration(rules,common);});
                savedEditor.SaveDraft();
                if(draftSaves!=1 || store.OutputMultiplier!=2 || store.ResolutionRules[6]!=-1)throw new Exception("Editor save did not commit draft.");
                foreach(int width in new[]{504,464}) {
                    FrameworkElement editorClient=(FrameworkElement)savedEditor.Window.Content;
                    editorClient.Measure(new Size(width,601));editorClient.Arrange(new Rect(0,0,width,601));editorClient.UpdateLayout();
                    foreach(string name in new[]{"CommonOutputChoice","ResolutionChoice0","ResolutionChoice6","ResolutionSaveButton","ResolutionCancelButton","ResolutionResetButton"}) {
                        var item=(FrameworkElement)savedEditor.Window.FindName(name);
                        var bounds=item.TransformToAncestor(editorClient).TransformBounds(new Rect(0,0,item.ActualWidth,item.ActualHeight));
                        if(bounds.Left<0 || bounds.Right>width || bounds.Bottom>601)throw new Exception("Resolution editor clipped: "+name);
                    }
                }
                System.Windows.Input.FocusManager.SetFocusedElement(savedEditor.Window,savedEditor.Choices[3]);
                if(!savedEditor.Choices[3].IsFocused || String.IsNullOrEmpty(System.Windows.Automation.AutomationProperties.GetName(savedEditor.Choices[3])))throw new Exception("Resolution choice focus/label missing.");
                var styledChoice=savedEditor.Choices[3];
                styledChoice.ApplyTemplate();
                var menu=styledChoice.Template.FindName("PART_Popup",styledChoice) as Popup;
                var trigger=styledChoice.Template.FindName("DropDownToggle",styledChoice) as ToggleButton;
                if(menu==null || menu.Child==null || trigger==null || trigger.Focusable || trigger.IsTabStop)
                    throw new Exception("Custom select must retain native popup and a single keyboard tab stop.");
                var openBinding=System.Windows.Data.BindingOperations.GetBinding(trigger,ToggleButton.IsCheckedProperty);
                if(openBinding==null || openBinding.Path.Path!="IsDropDownOpen" || openBinding.Mode!=System.Windows.Data.BindingMode.TwoWay)
                    throw new Exception("Select trigger must synchronize native open/close state.");
                var menuContent=(FrameworkElement)menu.Child;
                menuContent.Measure(new Size(260,Double.PositiveInfinity));menuContent.Arrange(new Rect(0,0,260,menuContent.DesiredSize.Height));menuContent.UpdateLayout();
                var selectedMenuItem=(ComboBoxItem)styledChoice.SelectedItem;selectedMenuItem.ApplyTemplate();
                var selectionMark=selectedMenuItem.Template.FindName("Check",selectedMenuItem) as System.Windows.Shapes.Path;
                if(selectionMark==null || selectionMark.Visibility!=Visibility.Visible)throw new Exception("Menu selection must remain visible without relying only on color.");
                savedEditor.Window.Close();mode.SelectedIndex=1;
                testController.PreviewState("error");
                if (!((TextBox)testWindow.FindName("DiagnosticsText")).Text.Contains("AnotherLongExampleFolder")) throw new Exception("Full action error details must remain available without expanding the footer.");
                foreach (int width in new int[] { 504, 464 })
                {
                    ((TabItem)testWindow.FindName("InterpolationTab")).IsSelected = true;
                    FrameworkElement client = (FrameworkElement)testWindow.Content;
                    client.Measure(new Size(width, 501)); client.Arrange(new Rect(0, 0, width, 501)); client.UpdateLayout();
                    TextBlock saveText = (TextBlock)testWindow.FindName("SaveNotice");
                    double footerTop = saveText.TransformToAncestor(client).Transform(new Point(0, 0)).Y - 8;
                    foreach (string controlName in new string[] { "EnabledToggle", "InputRate0", "InputRate5", "OutputRateSummary", "OutputModeChoice", "OutputValueChoice", "OutputValueLabel", "HeightAll", "Height1080", "Height1440", "OutputRateHint" })
                    {
                        FrameworkElement item = (FrameworkElement)testWindow.FindName(controlName);
                        Rect bounds = item.TransformToAncestor(client).TransformBounds(new Rect(0, 0, item.ActualWidth, item.ActualHeight));
                        if (bounds.Left < 0 || bounds.Right > width || bounds.Top < 0 || bounds.Bottom > footerTop) throw new Exception("Essential control clipped at " + width + ": " + controlName);
                    }
                    var hint=(TextBlock)testWindow.FindName("OutputRateHint");double shownHeight=hint.ActualHeight;
                    hint.Measure(new Size(hint.ActualWidth,Double.PositiveInfinity));
                    if(hint.DesiredSize.Height-hint.Margin.Top-hint.Margin.Bottom>shownHeight+0.5)throw new Exception("Output explanation text is internally clipped at "+width);
                }
                if(testWindow.FindName("EnhancementsTab")!=null || ((TabControl)testWindow.FindName("SettingsTabs")).Items.Count!=3)throw new Exception("Obsolete enhancements tab remains.");
                testController.ConfigurePackageUi(false);
                if(((Button)testWindow.FindName("UninstallButton")).Visibility!=Visibility.Collapsed)throw new Exception("Portable build exposes installer removal.");
                testController.ConfigurePackageUi(true);
                if(((Button)testWindow.FindName("UninstallButton")).Visibility!=Visibility.Visible)throw new Exception("Installed removal is hidden.");
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
                if(RegistrationInfo.CreateStartInfo(folder,false,true).Arguments!="--unregister" || RegistrationInfo.CreateStartInfo(folder,true,true).Arguments!="--unregister-machine")throw new Exception("Unregister routing is incorrect.");
                if (machineRegistration.RedirectStandardOutput || machineRegistration.RedirectStandardError) throw new Exception("Elevated shell execution cannot redirect streams.");
                File.WriteAllText(Path.Combine(folder, "passed.txt"), "Settings, status, transport labels, and registration routing tests passed. No registration process was launched.");
                return 0;
            }
            finally { Directory.Delete(folder, true); }
        }
    }
}



