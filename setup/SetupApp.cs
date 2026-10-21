using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Win32;
using QRCoder;

namespace StreamKitSetup
{
    internal static class Program
    {
        [DllImport("user32.dll")]
        private static extern bool SetProcessDPIAware();

        [STAThread]
        private static void Main()
        {
            try { SetProcessDPIAware(); } catch { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm());
        }
    }

    internal sealed class SetupForm : Form
    {
        private const string AppVersion = "0.1.0";
        private const string UserAgent =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

        private static readonly Color Bg = Color.FromArgb(11, 13, 20);
        private static readonly Color Card = Color.FromArgb(17, 21, 34);
        private static readonly Color Line = Color.FromArgb(42, 48, 66);
        private static readonly Color Fg = Color.FromArgb(238, 242, 255);
        private static readonly Color Dim = Color.FromArgb(138, 147, 170);
        private static readonly Color Green = Color.FromArgb(127, 224, 165);
        private static readonly Color Gold = Color.FromArgb(255, 207, 58);
        private static readonly Color Red = Color.FromArgb(255, 138, 138);

        private string _gameBin;
        private string _iniPath;
        private string _authPath;

        private TextBox _gameBox;
        private ComboBox _hostBox;
        private NumericUpDown _portBox;
        private TextBox _roomBox;
        private CheckBox _sendBox;
        private PictureBox _qrBox;
        private Label _qrStatus;
        private Label _authStatus;
        private Label _msg;
        private Button _qrButton;
        private ToggleCard _reqCard;
        private ToggleCard _dmCard;
        private Label _urlMain;
        private Label _urlChat;
        private Label _urlSetup;
        private Label _urlLan;
        private PictureBox _phoneQr;
        private bool _requests;
        private bool _danmaku;

        private string _qrKey;
        private Timer _pollTimer;
        private bool _polling;
        private CookieContainer _cookies;
        private string _autoRoomUid;

        public SetupForm()
        {
            Text = "Chunithm BililiveKit v" + AppVersion;
            ClientSize = new Size(960, 760);
            MinimumSize = new Size(900, 700);
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.Sizable;
            MaximizeBox = true;
            BackColor = Bg;
            ForeColor = Fg;
            Font = new Font("Microsoft YaHei UI", 9F);
            DoubleBuffered = true;

            Label title = MakeLabel("Chunithm BililiveKit", 22, 16, 17F, true);
            Controls.Add(title);
            Controls.Add(MakeLabel("v" + AppVersion + " · 先扫码登录、配置模块，再启动游戏", 22, 50, 9F, false, Dim));

            Panel gameCard = MakeCard(22, 82, 916, 68);
            Controls.Add(gameCard);
            gameCard.Controls.Add(MakeLabel("游戏 bin", 18, 26, 9F, false, Dim));
            _gameBox = new TextBox();
            _gameBox.Location = new Point(104, 22);
            _gameBox.Size = new Size(630, 28);
            _gameBox.ReadOnly = true;
            _gameBox.BackColor = Color.FromArgb(8, 10, 16);
            _gameBox.ForeColor = Fg;
            _gameBox.BorderStyle = BorderStyle.FixedSingle;
            gameCard.Controls.Add(_gameBox);
            Button choose = MakeButton("选择目录", 748, 20, 110);
            choose.Click += delegate { ChooseGame(); };
            gameCard.Controls.Add(choose);

            Panel loginCard = MakeCard(22, 162, 380, 354);
            Controls.Add(loginCard);
            loginCard.Controls.Add(MakeLabel("B站扫码登录", 18, 14, 11F, true));
            _qrBox = new PictureBox();
            _qrBox.Location = new Point(90, 48);
            _qrBox.Size = new Size(200, 200);
            _qrBox.SizeMode = PictureBoxSizeMode.Zoom;
            _qrBox.BackColor = Color.White;
            _qrBox.BorderStyle = BorderStyle.FixedSingle;
            loginCard.Controls.Add(_qrBox);
            _qrStatus = MakeLabel("点击“生成二维码”，手机扫码并确认。", 18, 254, 8.5F, false, Dim);
            _qrStatus.Size = new Size(344, 22);
            loginCard.Controls.Add(_qrStatus);
            _authStatus = MakeLabel("未登录", 18, 278, 8.5F, false, Red);
            _authStatus.Size = new Size(344, 22);
            loginCard.Controls.Add(_authStatus);
            _qrButton = MakeButton("生成二维码", 18, 302, 150);
            _qrButton.Click += delegate { GenerateQr(); };
            loginCard.Controls.Add(_qrButton);
            Button refreshAuth = MakeButton("刷新登录", 180, 302, 106);
            refreshAuth.Click += delegate { LoadState(); };
            loginCard.Controls.Add(refreshAuth);

            Panel moduleCard = MakeCard(420, 162, 518, 176);
            Controls.Add(moduleCard);
            moduleCard.Controls.Add(MakeLabel("功能开关", 18, 12, 11F, true));
            _reqCard = new ToggleCard();
            _reqCard.Title = "点歌板";
            _reqCard.Subtitle = "接收弹幕点歌指令";
            _reqCard.Location = new Point(18, 46);
            _reqCard.Size = new Size(238, 124);
            _reqCard.CheckedChanged += delegate { _requests = _reqCard.Checked; UpdateToggles(); };
            moduleCard.Controls.Add(_reqCard);
            _dmCard = new ToggleCard();
            _dmCard.Title = "弹幕接收";
            _dmCard.Subtitle = "连接 B站直播间";
            _dmCard.Location = new Point(268, 46);
            _dmCard.Size = new Size(238, 124);
            _dmCard.CheckedChanged += delegate { _danmaku = _dmCard.Checked; UpdateToggles(); };
            moduleCard.Controls.Add(_dmCard);

            Panel configCard = MakeCard(420, 350, 518, 166);
            Controls.Add(configCard);
            configCard.Controls.Add(MakeLabel("连接与直播间", 18, 12, 11F, true));
            configCard.Controls.Add(MakeLabel("监听地址", 18, 58, 8.5F, false, Dim));
            _hostBox = new ComboBox();
            _hostBox.DropDownStyle = ComboBoxStyle.DropDownList;
            _hostBox.Items.Add("127.0.0.1（仅本机）");
            _hostBox.Items.Add("0.0.0.0（局域网）");
            _hostBox.Location = new Point(98, 54);
            _hostBox.Size = new Size(200, 28);
            _hostBox.BackColor = Color.FromArgb(8, 10, 16);
            _hostBox.ForeColor = Fg;
            _hostBox.SelectedIndexChanged += delegate { UpdateUrls(); };
            configCard.Controls.Add(_hostBox);

            configCard.Controls.Add(MakeLabel("端口", 316, 58, 8.5F, false, Dim));
            _portBox = new NumericUpDown();
            _portBox.Minimum = 1;
            _portBox.Maximum = 65535;
            _portBox.Value = 8770;
            _portBox.Location = new Point(362, 54);
            _portBox.Size = new Size(110, 28);
            _portBox.BackColor = Color.FromArgb(8, 10, 16);
            _portBox.ForeColor = Fg;
            _portBox.ValueChanged += delegate { UpdateUrls(); };
            configCard.Controls.Add(_portBox);

            configCard.Controls.Add(MakeLabel("直播间号", 18, 100, 8.5F, false, Dim));
            _roomBox = new TextBox();
            _roomBox.Location = new Point(98, 96);
            _roomBox.Size = new Size(268, 28);
            _roomBox.BackColor = Color.FromArgb(8, 10, 16);
            _roomBox.ForeColor = Fg;
            _roomBox.BorderStyle = BorderStyle.FixedSingle;
            configCard.Controls.Add(_roomBox);
            Button autoRoom = MakeButton("自动获取", 374, 92, 98);
            autoRoom.Click += delegate
            {
                string uid = CookieValue(ReadText(_authPath), "DedeUserID");
                if (uid == "") SetMsg("请先扫码登录", true);
                else AutoRoom(uid, true);
            };
            configCard.Controls.Add(autoRoom);

            _sendBox = new CheckBox();
            _sendBox.Text = "允许手机网页发弹幕";
            _sendBox.Location = new Point(18, 138);
            _sendBox.Size = new Size(230, 24);
            _sendBox.ForeColor = Fg;
            configCard.Controls.Add(_sendBox);
            configCard.Controls.Add(MakeLabel("保存后重启游戏生效", 292, 140, 8F, false, Dim));

            Panel urlCard = MakeCard(22, 530, 916, 170);
            Controls.Add(urlCard);
            urlCard.Controls.Add(MakeLabel("网页 / OBS 地址", 18, 12, 11F, true));
            _urlMain = MakeLabel("", 18, 40, 9F, false, Fg);
            _urlMain.Size = new Size(640, 22);
            urlCard.Controls.Add(_urlMain);
            _urlChat = MakeLabel("", 18, 66, 9F, false, Fg);
            _urlChat.Size = new Size(640, 22);
            urlCard.Controls.Add(_urlChat);
            _urlSetup = MakeLabel("", 18, 92, 9F, false, Fg);
            _urlSetup.Size = new Size(640, 22);
            urlCard.Controls.Add(_urlSetup);
            _urlLan = MakeLabel("", 18, 122, 8.5F, false, Dim);
            _urlLan.Size = new Size(760, 22);
            urlCard.Controls.Add(_urlLan);
            Button copyUrl = MakeButton("复制 OBS 地址", 700, 24, 136);
            copyUrl.Click += delegate
            {
                try { Clipboard.SetText("http://127.0.0.1:" + ((int)_portBox.Value) + "/"); } catch { }
                copyUrl.Text = "已复制";
                Timer t = new Timer();
                t.Interval = 1200;
                t.Tick += delegate { copyUrl.Text = "复制 OBS 地址"; t.Stop(); t.Dispose(); };
                t.Start();
            };
            urlCard.Controls.Add(copyUrl);

            _phoneQr = new PictureBox();
            _phoneQr.Location = new Point(820, 102);
            _phoneQr.Size = new Size(48, 48);
            _phoneQr.SizeMode = PictureBoxSizeMode.Zoom;
            _phoneQr.BackColor = Color.White;
            _phoneQr.BorderStyle = BorderStyle.FixedSingle;
            urlCard.Controls.Add(_phoneQr);
            urlCard.Controls.Add(MakeLabel("手机扫码", 808, 152, 7.5F, false, Dim));

            Button save = MakeButton("只保存配置", 22, 710, 130);
            save.Click += delegate { SaveConfig(false); };
            Controls.Add(save);
            Button launch = MakeButton("保存并启动游戏", 164, 710, 180);
            launch.BackColor = Color.FromArgb(36, 92, 58);
            launch.FlatAppearance.BorderColor = Color.FromArgb(90, 200, 130);
            launch.Click += delegate { SaveConfig(true); };
            Controls.Add(launch);
            Button check = MakeButton("运行检查", 360, 710, 110);
            check.Click += delegate { ShowRuntimeCheck(); };
            Controls.Add(check);
            _msg = MakeLabel("", 486, 716, 9F, false, Green);
            _msg.Size = new Size(320, 22);
            Controls.Add(_msg);
            Button close = MakeButton("关闭", 836, 710, 102);
            close.Click += delegate { Close(); };
            Controls.Add(close);

            _pollTimer = new Timer();
            _pollTimer.Interval = 2000;
            _pollTimer.Tick += delegate { PollQr(); };

            Load += delegate
            {
                DetectGame();
                LoadState();
                UpdateUrls();
            };
            FormClosing += delegate { _pollTimer.Stop(); };
        }

        private static Label MakeLabel(string text, int x, int y, float size, bool bold)
        {
            return MakeLabel(text, x, y, size, bold, Fg);
        }

        private static Label MakeLabel(string text, int x, int y, float size, bool bold, Color color)
        {
            Label l = new Label();
            l.Text = text;
            l.Location = new Point(x, y);
            l.AutoSize = true;
            l.ForeColor = color;
            l.Font = new Font("Microsoft YaHei UI", size, bold ? FontStyle.Bold : FontStyle.Regular);
            return l;
        }

        private static Panel MakeCard(int x, int y, int w, int h)
        {
            Panel p = new RoundPanel();
            p.Location = new Point(x, y);
            p.Size = new Size(w, h);
            p.BackColor = Card;
            return p;
        }

        private static Button MakeButton(string text, int x, int y, int w)
        {
            Button b = new RoundButton();
            b.Text = text;
            b.Location = new Point(x, y);
            b.Size = new Size(w, 32);
            b.FlatStyle = FlatStyle.Flat;
            b.FlatAppearance.BorderColor = Line;
            b.BackColor = Color.FromArgb(28, 33, 48);
            b.ForeColor = Fg;
            b.Cursor = Cursors.Hand;
            return b;
        }

        private void UpdateToggles()
        {
            if (_reqCard != null) _reqCard.Checked = _requests;
            if (_dmCard != null) _dmCard.Checked = _danmaku;
        }

        private void UpdateUrls()
        {
            int port = (int)_portBox.Value;
            _urlMain.Text = "OBS 叠加层：http://127.0.0.1:" + port + "/";
            _urlChat.Text = "独立弹幕栏：http://127.0.0.1:" + port + "/chat.html";
            _urlSetup.Text = "运行检查：点下方「运行检查」按钮";
            string lan = GetLanIp();
            string phone = "";
            if (lan != "") phone = "http://" + lan + ":" + port + "/";
            _urlLan.Text = phone != ""
                ? (_hostBox.SelectedIndex == 1
                    ? "手机/平板： " + phone
                    : "手机地址（先把监听地址改为 0.0.0.0）： " + phone)
                : "手机/平板：需要电脑连接到局域网";
            if (_phoneQr != null)
            {
                if (_phoneQr.Image != null)
                {
                    Image old = _phoneQr.Image;
                    _phoneQr.Image = null;
                    old.Dispose();
                }
                if (phone != "")
                {
                    try
                    {
                        using (QRCodeGenerator gen = new QRCodeGenerator())
                        using (QRCodeData data = gen.CreateQrCode(phone, QRCodeGenerator.ECCLevel.M))
                        using (QRCode qr = new QRCode(data))
                            _phoneQr.Image = qr.GetGraphic(5);
                    }
                    catch { }
                }
            }
        }

        private static string GetLanIp()
        {
            string best = "";
            int bestScore = -1;
            try
            {
                foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != OperationalStatus.Up) continue;
                    if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                    string name = ni.Name.ToLowerInvariant();
                    if (name.Contains("vpn") || name.Contains("radmin") || name.Contains("tap") ||
                        name.Contains("vethernet") || name.Contains("virtual") || name.Contains("loopback"))
                        continue;
                    int score = 0;
                    if (ni.NetworkInterfaceType == NetworkInterfaceType.Ethernet ||
                        ni.NetworkInterfaceType == NetworkInterfaceType.GigabitEthernet ||
                        ni.NetworkInterfaceType == NetworkInterfaceType.Wireless80211) score = 2;
                    foreach (UnicastIPAddressInformation ip in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ip.Address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) continue;
                        if (IPAddress.IsLoopback(ip.Address)) continue;
                        if (ip.Address.ToString().StartsWith("169.254.")) continue;
                        if (score > bestScore)
                        {
                            bestScore = score;
                            best = ip.Address.ToString();
                        }
                    }
                }
            }
            catch { }
            return best;
        }

        private static string SavedGameBin()
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\CHUNITHM StreamKit"))
                {
                    if (k != null)
                    {
                        string v = Convert.ToString(k.GetValue("GameBin"));
                        if (!string.IsNullOrEmpty(v)) return v;
                    }
                }
            }
            catch { }
            try
            {
                string p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "CHUNITHM StreamKit", "install.ini");
                if (File.Exists(p))
                {
                    string v = File.ReadAllText(p, Encoding.UTF8).Trim();
                    if (!string.IsNullOrEmpty(v)) return v;
                }
            }
            catch { }
            return "";
        }

        internal static void SaveGameBin(string bin)
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\CHUNITHM StreamKit"))
                {
                    if (k != null) k.SetValue("GameBin", bin);
                }
            }
            catch { }
            try
            {
                string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "CHUNITHM StreamKit");
                Directory.CreateDirectory(dir);
                File.WriteAllText(Path.Combine(dir, "install.ini"), bin, new UTF8Encoding(false));
            }
            catch { }
        }

        private void DetectGame()
        {
            string saved = SavedGameBin();
            if (!string.IsNullOrEmpty(saved) && File.Exists(Path.Combine(saved, "chusanApp.exe")))
            {
                SetGameBin(saved);
                if (!File.Exists(Path.Combine(saved, "chunithm_streamkit.asi")))
                    OfferInstall();
                return;
            }
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            if (File.Exists(Path.Combine(baseDir, "chusanApp.exe")))
            {
                SetGameBin(baseDir);
                SaveGameBin(baseDir);
                return;
            }
            ChooseGame();
            if (!string.IsNullOrEmpty(_gameBin)) OfferInstall();
        }

        private void ChooseGame()
        {
            using (FolderBrowserDialog dlg = new FolderBrowserDialog())
            {
                dlg.Description = "选择游戏的 bin 目录（包含 chusanApp.exe）";
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                if (!File.Exists(Path.Combine(dlg.SelectedPath, "chusanApp.exe")))
                {
                    MessageBox.Show(this, "这个目录里没有 chusanApp.exe，请选择游戏 bin 目录。",
                        "目录不对", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                SetGameBin(dlg.SelectedPath);
                SaveGameBin(_gameBin);
            }
        }

        private void OfferInstall()
        {
            if (string.IsNullOrEmpty(_gameBin)) return;
            if (File.Exists(Path.Combine(_gameBin, "chunithm_streamkit.asi"))) return;
            string payload = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bin");
            if (!File.Exists(Path.Combine(payload, "chunithm_streamkit.asi")))
            {
                MessageBox.Show(this, "安装包不完整：找不到 bin\\chunithm_streamkit.asi。", "安装包错误",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (System.Diagnostics.Process.GetProcessesByName("chusanApp").Length > 0)
            {
                MessageBox.Show(this, "游戏正在运行，请先关闭游戏再安装插件。", "游戏未关闭",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            DialogResult r = MessageBox.Show(this,
                "将把插件文件复制到：\n" + _gameBin +
                "\n\n已有的 ini、auth.txt、songrequest.tsv 不会被覆盖。\n是否继续？",
                "安装插件", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (r != DialogResult.Yes) return;
            try
            {
                CopyPayload(payload, _gameBin);
                SetMsg("插件文件已安装", false);
                SaveGameBin(_gameBin);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "安装失败：" + ex.Message, "安装失败",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private static void CopyPayload(string src, string dst)
        {
            foreach (string dir in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
            {
                string rel = dir.Substring(src.Length).TrimStart('\\');
                Directory.CreateDirectory(Path.Combine(dst, rel));
            }
            foreach (string file in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string rel = file.Substring(src.Length).TrimStart('\\');
                string name = Path.GetFileName(file);
                string target = Path.Combine(dst, rel);
                if (File.Exists(target) &&
                    (name.Equals("chunithm_streamkit.ini", StringComparison.OrdinalIgnoreCase) ||
                     name.Equals("auth.txt", StringComparison.OrdinalIgnoreCase) ||
                     name.Equals("songrequest.tsv", StringComparison.OrdinalIgnoreCase)))
                    continue;
                string parent = Path.GetDirectoryName(target);
                if (!Directory.Exists(parent)) Directory.CreateDirectory(parent);
                File.Copy(file, target, true);
            }
        }

        private void SetGameBin(string bin)
        {
            _gameBin = bin;
            _iniPath = Path.Combine(bin, "chunithm_streamkit", "chunithm_streamkit.ini");
            _authPath = Path.Combine(bin, "chunithm_streamkit", "auth.txt");
            _gameBox.Text = bin;
        }

        private string ReadText(string path)
        {
            try { return File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8) : ""; }
            catch { return ""; }
        }

        private void WriteText(string path, string text)
        {
            string dir = Path.GetDirectoryName(path);
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            string tmp = path + ".tmp";
            File.WriteAllText(tmp, text, new UTF8Encoding(false));
            if (File.Exists(path)) File.Delete(path);
            File.Move(tmp, path);
        }

        private static string IniGet(string text, string section, string key)
        {
            string[] lines = text.Replace("\r\n", "\n").Split('\n');
            bool inSec = false;
            foreach (string raw in lines)
            {
                string line = raw.Trim();
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    inSec = line.Substring(1, line.Length - 2) == section;
                    continue;
                }
                if (!inSec || line == "" || line.StartsWith(";") || line.StartsWith("#")) continue;
                int eq = line.IndexOf('=');
                if (eq < 0) continue;
                if (line.Substring(0, eq).Trim() == key) return line.Substring(eq + 1).Trim();
            }
            return "";
        }

        private static string IniSet(string text, string section, string key, string value)
        {
            List<string> lines = new List<string>(text.Replace("\r\n", "\n").Split('\n'));
            int sec = -1, end = lines.Count;
            for (int i = 0; i < lines.Count; i++)
            {
                string line = lines[i].Trim();
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    if (sec < 0 && line.Substring(1, line.Length - 2) == section) sec = i;
                    else if (sec >= 0) { end = i; break; }
                }
            }
            if (sec < 0)
            {
                if (lines.Count > 0 && lines[lines.Count - 1] != "") lines.Add("");
                lines.Add("[" + section + "]");
                lines.Add(key + "=" + value);
            }
            else
            {
                int found = -1;
                for (int i = sec + 1; i < end; i++)
                {
                    string line = lines[i].Trim();
                    if (line == "" || line.StartsWith(";") || line.StartsWith("#")) continue;
                    int eq = line.IndexOf('=');
                    if (eq >= 0 && line.Substring(0, eq).Trim() == key) { found = i; break; }
                }
                if (found >= 0) lines[found] = key + "=" + value;
                else lines.Insert(end, key + "=" + value);
            }
            return string.Join("\n", lines.ToArray()) + "\n";
        }

        private void LoadState()
        {
            if (string.IsNullOrEmpty(_iniPath))
            {
                _authStatus.Text = "未选择游戏目录";
                return;
            }
            string ini = ReadText(_iniPath);
            string host = IniGet(ini, "server", "host");
            _hostBox.SelectedIndex = host == "0.0.0.0" ? 1 : 0;
            int port;
            if (!int.TryParse(IniGet(ini, "server", "port"), out port) || port < 1 || port > 65535) port = 8770;
            _portBox.Value = port;
            _danmaku = IniGet(ini, "danmaku", "enabled") == "1";
            _roomBox.Text = IniGet(ini, "danmaku", "room_id");
            _sendBox.Checked = IniGet(ini, "danmaku", "allow_send") == "1";
            _requests = IniGet(ini, "songrequest", "enabled") == "1";
            UpdateToggles();
            UpdateUrls();

            string auth = ReadText(_authPath);
            string uid = CookieValue(auth, "DedeUserID");
            _authStatus.Text = auth.Length > 0 && uid != ""
                ? "已保存登录 Cookie · UID " + uid
                : "未登录，请扫码";
            _authStatus.ForeColor = auth.Length > 0 && uid != "" ? Green : Red;
            if (uid != "" && _roomBox.Text.Trim() == "") AutoRoom(uid);
        }

        private void AutoRoom(string uid, bool force = false)
        {
            if (!force && _autoRoomUid == uid) return;
            _autoRoomUid = uid;
            Task.Factory.StartNew(delegate
            {
                string room = "";
                try
                {
                    string url = "https://api.live.bilibili.com/room/v1/Room/getRoomInfoOld?mid=" + uid;
                    string body = HttpGet(url, "https://live.bilibili.com/", ref _cookies);
                    JavaScriptSerializer js = new JavaScriptSerializer();
                    Dictionary<string, object> root = js.Deserialize<Dictionary<string, object>>(body);
                    if (root.ContainsKey("data") && root["data"] != null)
                    {
                        Dictionary<string, object> data = (Dictionary<string, object>)root["data"];
                        if (data.ContainsKey("roomid")) room = Convert.ToString(data["roomid"]);
                    }
                }
                catch { }
                BeginInvoke((Action)delegate
                {
                    if (room != "" && room != "0" && (force || _roomBox.Text.Trim() == ""))
                    {
                        _roomBox.Text = room;
                        _qrStatus.Text = "已自动获取直播间：" + room + "，请确认后保存";
                    }
                    else if (room == "" || room == "0")
                    {
                        _qrStatus.Text = "未自动找到直播间，请手动填写直播间号";
                    }
                });
            });
        }

        private static string CookieValue(string jar, string name)
        {
            string[] parts = jar.Split(';');
            foreach (string p in parts)
            {
                string s = p.Trim();
                int eq = s.IndexOf('=');
                if (eq > 0 && s.Substring(0, eq) == name) return s.Substring(eq + 1);
            }
            return "";
        }

        private void SaveConfig(bool launch)
        {
            if (string.IsNullOrEmpty(_iniPath))
            {
                SetMsg("请先选择游戏目录", true);
                return;
            }
            string room = _roomBox.Text.Trim();
            foreach (char c in room)
            {
                if (!char.IsDigit(c)) { SetMsg("直播间号必须是数字", true); return; }
            }
            string text = ReadText(_iniPath);
            string host = _hostBox.SelectedIndex == 1 ? "0.0.0.0" : "127.0.0.1";
            text = IniSet(text, "server", "host", host);
            text = IniSet(text, "server", "port", ((int)_portBox.Value).ToString());
            text = IniSet(text, "danmaku", "enabled", _danmaku ? "1" : "0");
            text = IniSet(text, "danmaku", "room_id", room);
            text = IniSet(text, "danmaku", "allow_send", _sendBox.Checked ? "1" : "0");
            text = IniSet(text, "songrequest", "enabled", _requests ? "1" : "0");
            try
            {
                WriteText(_iniPath, text);
                SetMsg("配置已保存", false);
            }
            catch (Exception ex)
            {
                SetMsg("保存失败：" + ex.Message, true);
                return;
            }
            if (launch) LaunchGame();
        }

        private void LaunchGame()
        {
            try
            {
                string bat = Path.Combine(_gameBin, "start.bat");
                if (!File.Exists(bat))
                {
                    SetMsg("找不到 start.bat", true);
                    return;
                }
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                {
                    FileName = bat,
                    WorkingDirectory = _gameBin,
                    UseShellExecute = true
                });
                SetMsg("已启动游戏", false);
            }
            catch (Exception ex)
            {
                SetMsg("启动失败：" + ex.Message, true);
            }
        }

        private void ShowRuntimeCheck()
        {
            using (RuntimeStatusForm f = new RuntimeStatusForm((int)_portBox.Value))
                f.ShowDialog(this);
        }

        private void SetMsg(string text, bool error)
        {
            _msg.Text = text;
            _msg.ForeColor = error ? Red : Green;
        }

        private void GenerateQr()
        {
            _qrButton.Enabled = false;
            _qrStatus.Text = "正在生成二维码…";
            _pollTimer.Stop();
            Task.Factory.StartNew(delegate
            {
                try
                {
                    string body = HttpGet("https://passport.bilibili.com/x/passport-login/web/qrcode/generate", null, ref _cookies);
                    JavaScriptSerializer js = new JavaScriptSerializer();
                    Dictionary<string, object> root = js.Deserialize<Dictionary<string, object>>(body);
                    if (Convert.ToInt32(root["code"]) != 0) throw new Exception("生成失败");
                    Dictionary<string, object> data = (Dictionary<string, object>)root["data"];
                    string url = Convert.ToString(data["url"]);
                    string key = Convert.ToString(data["qrcode_key"]);
                    BeginInvoke((Action)delegate
                    {
                        _qrKey = key;
                        using (QRCodeGenerator gen = new QRCodeGenerator())
                        using (QRCodeData qrData = gen.CreateQrCode(url, QRCodeGenerator.ECCLevel.M))
                        using (QRCode qr = new QRCode(qrData))
                        {
                            _qrBox.Image = qr.GetGraphic(7);
                        }
                        _qrStatus.Text = "请用手机 B站扫码，并在手机上确认";
                        _qrButton.Enabled = true;
                        _pollTimer.Start();
                    });
                }
                catch (Exception ex)
                {
                    BeginInvoke((Action)delegate
                    {
                        _qrStatus.Text = "生成失败：" + ex.Message;
                        _qrButton.Enabled = true;
                    });
                }
            });
        }

        private void PollQr()
        {
            if (_polling || string.IsNullOrEmpty(_qrKey)) return;
            _polling = true;
            string key = _qrKey;
            Task.Factory.StartNew(delegate
            {
                string statusText = "";
                bool done = false;
                try
                {
                    string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" +
                                 Uri.EscapeDataString(key);
                    string body = HttpGet(url, null, ref _cookies);
                    JavaScriptSerializer js = new JavaScriptSerializer();
                    Dictionary<string, object> root = js.Deserialize<Dictionary<string, object>>(body);
                    Dictionary<string, object> data = root.ContainsKey("data") ? (Dictionary<string, object>)root["data"] : null;
                    int code = data != null && data.ContainsKey("code") ? Convert.ToInt32(data["code"]) : -1;
                    if (code == 0)
                    {
                        string jar = BuildJar(_cookies);
                        if (CookieValue(jar, "SESSDATA") == "")
                        {
                            statusText = "登录成功但未拿到 Cookie";
                        }
                        else
                        {
                            WriteText(_authPath, jar);
                            statusText = "登录成功，已保存 Cookie · UID " + CookieValue(jar, "DedeUserID");
                            done = true;
                        }
                    }
                    else if (code == 86101) statusText = "等待扫码";
                    else if (code == 86090) statusText = "已扫码，请在手机上确认";
                    else if (code == 86095) statusText = "二维码已过期，请重新生成";
                    else statusText = "登录状态 " + code;
                }
                catch (Exception ex)
                {
                    statusText = "轮询失败：" + ex.Message;
                }
                BeginInvoke((Action)delegate
                {
                    _qrStatus.Text = statusText;
                    _polling = false;
                    if (done)
                    {
                        _pollTimer.Stop();
                        _qrKey = "";
                        LoadState();
                    }
                });
            });
        }

        private static string BuildJar(CookieContainer jar)
        {
            List<string> parts = new List<string>();
            foreach (string u in new string[] { "https://www.bilibili.com", "https://passport.bilibili.com" })
            {
                try
                {
                    foreach (Cookie c in jar.GetCookies(new Uri(u)))
                    {
                        bool exists = false;
                        foreach (string p in parts)
                            if (p.StartsWith(c.Name + "=")) { exists = true; break; }
                        if (!exists && !string.IsNullOrEmpty(c.Value)) parts.Add(c.Name + "=" + c.Value);
                    }
                }
                catch { }
            }
            return string.Join("; ", parts.ToArray());
        }

        private static string HttpGet(string url, string referer, ref CookieContainer cookies)
        {
            if (cookies == null) cookies = new CookieContainer();
            HttpWebRequest req = (HttpWebRequest)WebRequest.Create(url);
            req.UserAgent = UserAgent;
            req.Referer = string.IsNullOrEmpty(referer) ? "https://www.bilibili.com/" : referer;
            req.CookieContainer = cookies;
            req.Timeout = 15000;
            req.ReadWriteTimeout = 15000;
            req.AllowAutoRedirect = true;
            using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
            using (StreamReader sr = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                return sr.ReadToEnd();
        }
    }

    internal sealed class RuntimeStatusForm : Form
    {
        private readonly int _port;
        private TextBox _box;
        private Label _hint;

        public RuntimeStatusForm(int port)
        {
            _port = port;
            Text = "运行检查";
            ClientSize = new Size(560, 440);
            StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            BackColor = Color.FromArgb(11, 13, 20);
            ForeColor = Color.FromArgb(238, 242, 255);
            Font = new Font("Microsoft YaHei UI", 9F);

            Label t = new Label();
            t.Text = "Chunithm BililiveKit 运行检查";
            t.Font = new Font("Microsoft YaHei UI", 14F, FontStyle.Bold);
            t.AutoSize = true;
            t.Location = new Point(18, 14);
            Controls.Add(t);

            _box = new TextBox();
            _box.Location = new Point(18, 50);
            _box.Size = new Size(524, 300);
            _box.Multiline = true;
            _box.ReadOnly = true;
            _box.ScrollBars = ScrollBars.Vertical;
            _box.BackColor = Color.FromArgb(17, 21, 34);
            _box.ForeColor = Color.FromArgb(238, 242, 255);
            _box.BorderStyle = BorderStyle.FixedSingle;
            _box.Font = new Font("Microsoft YaHei UI", 10F);
            Controls.Add(_box);

            _hint = new Label();
            _hint.Location = new Point(18, 360);
            _hint.Size = new Size(524, 24);
            _hint.ForeColor = Color.FromArgb(138, 147, 170);
            Controls.Add(_hint);

            Button refresh = new Button();
            refresh.Text = "刷新";
            refresh.Location = new Point(330, 392);
            refresh.Size = new Size(100, 30);
            refresh.FlatStyle = FlatStyle.Flat;
            refresh.FlatAppearance.BorderColor = Color.FromArgb(42, 48, 66);
            refresh.BackColor = Color.FromArgb(28, 33, 48);
            refresh.ForeColor = Color.FromArgb(238, 242, 255);
            refresh.Click += delegate { Check(); };
            Controls.Add(refresh);

            Button close = new Button();
            close.Text = "关闭";
            close.Location = new Point(442, 392);
            close.Size = new Size(100, 30);
            close.FlatStyle = FlatStyle.Flat;
            close.FlatAppearance.BorderColor = Color.FromArgb(42, 48, 66);
            close.BackColor = Color.FromArgb(28, 33, 48);
            close.ForeColor = Color.FromArgb(238, 242, 255);
            close.Click += delegate { Close(); };
            Controls.Add(close);

            Load += delegate { Check(); };
        }

        private void Check()
        {
            _box.Text = "正在连接 http://127.0.0.1:" + _port + "/ …";
            _hint.Text = "";
            try
            {
                HttpWebRequest req = (HttpWebRequest)WebRequest.Create("http://127.0.0.1:" + _port + "/diag");
                req.Timeout = 4000;
                req.ReadWriteTimeout = 4000;
                using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
                using (StreamReader sr = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                {
                    JavaScriptSerializer js = new JavaScriptSerializer();
                    Dictionary<string, object> d = js.Deserialize<Dictionary<string, object>>(sr.ReadToEnd());
                    string text = "";
                    text += "HTTP 服务     正常\r\n";
                    text += "插件版本      " + (d.ContainsKey("version") ? d["version"] : "?") + "\r\n\r\n";
                    text += "点歌板        " + (Bool(d, "requests") ? "已开启" : "关闭") + "\r\n";
                    text += "弹幕连接      " + (Bool(d, "danmaku_running") ? "已连接" : "未连接") + "\r\n";
                    text += "当前房间      " + (d.ContainsKey("room") ? d["room"] : "0") + "\r\n\r\n";
                    text += "B站登录       " + (Bool(d, "logged_in") ? "已登录" : "未登录") + "\r\n";
                    text += "登录 UID      " + (d.ContainsKey("uid") ? d["uid"] : "0") + "\r\n";
                    text += "网页发弹幕    " + (Bool(d, "send_enabled") ? "已开启" : "关闭") + "\r\n\r\n";
                    text += "本地曲库      " + (d.ContainsKey("db") ? d["db"] : "0") + " 首\r\n";
                    text += "别名数据      " + (d.ContainsKey("aliases") ? d["aliases"] : "0") + " 条\r\n";
                    _box.Text = text;
                    _hint.Text = "检查时间 " + DateTime.Now.ToString("HH:mm:ss");
                }
            }
            catch (Exception ex)
            {
                _box.Text = "无法连接插件服务。\r\n\r\n" +
                    "可能原因：\r\n" +
                    "1. 游戏还没有启动\r\n" +
                    "2. ASI 没有加载\r\n" +
                    "3. 端口不是 " + _port + "\r\n\r\n" +
                    "详细信息：" + ex.Message;
                _hint.Text = "先在游戏里确认插件正常加载，再点刷新。";
            }
        }

        private static bool Bool(Dictionary<string, object> d, string key)
        {
            if (!d.ContainsKey(key)) return false;
            object v = d[key];
            return v is bool && (bool)v;
        }
    }

    internal class RoundPanel : Panel
    {
        public int Radius = 14;
        public RoundPanel()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);
            BackColor = Color.FromArgb(17, 21, 34);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            Rectangle r = ClientRectangle;
            r.Width--; r.Height--;
            using (System.Drawing.Drawing2D.GraphicsPath p = RoundPath(r, Radius))
            {
                using (SolidBrush b = new SolidBrush(BackColor)) e.Graphics.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(42, 48, 66))) e.Graphics.DrawPath(pen, p);
            }
        }
        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            Rectangle r = ClientRectangle;
            r.Width--; r.Height--;
            using (System.Drawing.Drawing2D.GraphicsPath p = RoundPath(r, Radius)) Region = new Region(p);
        }
        internal static System.Drawing.Drawing2D.GraphicsPath RoundPath(Rectangle r, int radius)
        {
            int d = radius * 2;
            System.Drawing.Drawing2D.GraphicsPath p = new System.Drawing.Drawing2D.GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }
    }

    internal sealed class ToggleCard : Panel
    {
        public string Title = "";
        public string Subtitle = "";
        private bool _checked;
        private bool _hover;
        public event EventHandler CheckedChanged;
        public bool Checked
        {
            get { return _checked; }
            set
            {
                if (_checked == value) return;
                _checked = value;
                Invalidate();
            }
        }
        public ToggleCard()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);
            Cursor = Cursors.Hand;
        }
        protected override void OnClick(EventArgs e)
        {
            _checked = !_checked;
            Invalidate();
            if (CheckedChanged != null) CheckedChanged(this, EventArgs.Empty);
            base.OnClick(e);
        }
        protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hover = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            Rectangle r = ClientRectangle;
            r.Width--; r.Height--;
            Color bg = _checked ? Color.FromArgb(22, 64, 42) : Color.FromArgb(24, 29, 43);
            Color border = _checked ? Color.FromArgb(64, 160, 105) : Color.FromArgb(48, 55, 74);
            if (_hover) bg = _checked ? Color.FromArgb(26, 74, 48) : Color.FromArgb(31, 37, 54);
            using (System.Drawing.Drawing2D.GraphicsPath p = RoundPanel.RoundPath(r, 12))
            {
                using (SolidBrush b = new SolidBrush(bg)) e.Graphics.FillPath(b, p);
                using (Pen pen = new Pen(border, 1.2f)) e.Graphics.DrawPath(pen, p);
            }
            using (Font ft = new Font("Microsoft YaHei UI", 11F, FontStyle.Bold))
            using (Font fs = new Font("Microsoft YaHei UI", 8F))
            using (Font fv = new Font("Microsoft YaHei UI", 8.5F, FontStyle.Bold))
            {
                e.Graphics.DrawString(Title, ft, Brushes.White, 14, 12);
                e.Graphics.DrawString(Subtitle, fs, new SolidBrush(Color.FromArgb(160, 169, 190)), 14, 40);
                e.Graphics.DrawString(_checked ? "已开启 · 点击关闭" : "已关闭 · 点击开启",
                    fv, new SolidBrush(_checked ? Color.FromArgb(127, 224, 165) : Color.FromArgb(138, 147, 170)),
                    14, 68);
            }
            int sx = r.Right - 54, sy = r.Bottom - 30;
            using (SolidBrush track = new SolidBrush(_checked ? Color.FromArgb(127, 224, 165) : Color.FromArgb(70, 77, 96)))
                e.Graphics.FillRectangle(track, sx, sy, 40, 20);
            using (SolidBrush knob = new SolidBrush(Color.White))
                e.Graphics.FillEllipse(knob, _checked ? sx + 21 : sx + 2, sy + 2, 16, 16);
        }
    }

    internal sealed class RoundButton : Button
    {
        public RoundButton()
        {
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderColor = Color.FromArgb(42, 48, 66);
            BackColor = Color.FromArgb(28, 33, 48);
            ForeColor = Color.FromArgb(238, 242, 255);
            Cursor = Cursors.Hand;
        }
        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            Rectangle r = ClientRectangle;
            r.Width--; r.Height--;
            using (System.Drawing.Drawing2D.GraphicsPath p = RoundPanel.RoundPath(r, 7)) Region = new Region(p);
        }
    }
}

