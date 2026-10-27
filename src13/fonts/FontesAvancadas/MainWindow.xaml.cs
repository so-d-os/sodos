using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace FontesAvancadas;

public partial class MainWindow : Window
{
    private readonly string _dir = @"C:\sodos\fonts";
    private readonly string _phrasePath = @"C:\sodos\phrase.txt";
    private readonly string _rawPath = @"C:\sodos\preview.raw";
    private readonly DispatcherTimer _timer;
    private int _token;
    private bool _ready;

    public MainWindow()
    {
        InitializeComponent();
        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(180) };
        _timer.Tick += (_, _) => { _timer.Stop(); _ = RenderAsync(); };
        Loaded += (_, _) => LoadFonts();
    }

    private sealed class Face
    {
        public string Path { get; init; } = "";
        public string Label { get; set; } = "";
        public override string ToString() => Label;
    }

    private void LoadFonts()
    {
        FontList.Items.Clear();
        if (Directory.Exists(_dir))
        {
            foreach (var path in Directory.GetFiles(_dir, "*.ttf").OrderBy(Path.GetFileName))
            {
                FontList.Items.Add(new Face
                {
                    Path = path,
                    Label = Path.GetFileNameWithoutExtension(path)
                });
            }
        }
        _ready = true;
        if (FontList.Items.Count > 0)
            FontList.SelectedIndex = 0;
        else
            Status.Text = "Nenhuma fonte em C:\\sodos\\fonts";
    }

    private void FontList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready) return;
        Schedule();
    }

    private void Phrase_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (!_ready) return;
        Schedule();
    }

    private void SizeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (SizeLabel != null)
            SizeLabel.Text = ((int)SizeSlider.Value) + " pontos";
        if (!_ready) return;
        Schedule();
    }

    private void Schedule()
    {
        _timer.Stop();
        _timer.Start();
    }

    private static string LinuxPath(string win)
    {
        var full = Path.GetFullPath(win);
        if (full.Length >= 2 && full[1] == ':')
            return "/mnt/" + char.ToLowerInvariant(full[0]) + full.Substring(2).Replace('\\', '/');
        return full.Replace('\\', '/');
    }

    private async Task RenderAsync()
    {
        if (FontList.SelectedItem is not Face face) return;
        int token = ++_token;
        int px = (int)SizeSlider.Value;
        string phrase = Phrase.Text ?? "";
        if (phrase.Length == 0) phrase = " ";
        Status.Text = "Renderizando…";
        try
        {
            await File.WriteAllTextAsync(_phrasePath, phrase, new UTF8Encoding(false));
            var psi = new ProcessStartInfo
            {
                FileName = "wsl.exe",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            };
            psi.ArgumentList.Add("-e");
            psi.ArgumentList.Add("/mnt/c/sodos/fontrender");
            psi.ArgumentList.Add(LinuxPath(face.Path));
            psi.ArgumentList.Add("@" + LinuxPath(_phrasePath));
            psi.ArgumentList.Add(px.ToString());
            psi.ArgumentList.Add(LinuxPath(_rawPath));
            using var proc = Process.Start(psi);
            if (proc == null) throw new InvalidOperationException("wsl nao iniciou");
            string stdout = await proc.StandardOutput.ReadToEndAsync();
            string stderr = await proc.StandardError.ReadToEndAsync();
            await proc.WaitForExitAsync();
            if (token != _token) return;
            if (proc.ExitCode != 0)
            {
                Status.Text = stderr.Length > 0 ? stderr.Trim() : "fontrender saiu com codigo " + proc.ExitCode;
                return;
            }
            ApplyMeta(stdout, face);
            Preview.Source = LoadRaw(_rawPath);
            Status.Text = "";
        }
        catch (Exception ex)
        {
            if (token == _token) Status.Text = ex.Message;
        }
    }

    private void ApplyMeta(string stdout, Face face)
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var line in stdout.Split('\n'))
        {
            int tab = line.IndexOf('\t');
            if (tab <= 0) continue;
            map[line.Substring(0, tab).Trim()] = line.Substring(tab + 1).Trim();
        }
        string name = map.GetValueOrDefault("NAME") ?? face.Label;
        if (name.Length > 0) face.Label = name;
        FontList.Items.Refresh();
        Crumb.Text = "Personalização  >  Fontes  >  " + name;
        MetaName.Text = map.GetValueOrDefault("FULL") ?? name;
        MetaFile.Text = face.Path;
        MetaVersion.Text = map.GetValueOrDefault("VERSION") ?? "";
        MetaCopy.Text = map.GetValueOrDefault("COPYRIGHT") ?? "";
        MetaTm.Text = map.GetValueOrDefault("TRADEMARK") ?? "";
    }

    private static BitmapSource LoadRaw(string path)
    {
        byte[] data = File.ReadAllBytes(path);
        int nl = Array.IndexOf(data, (byte)'\n');
        int nl2 = Array.IndexOf(data, (byte)'\n', nl + 1);
        string wh = Encoding.ASCII.GetString(data, nl + 1, nl2 - nl - 1);
        string[] parts = wh.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        int w = int.Parse(parts[0]);
        int h = int.Parse(parts[1]);
        int start = nl2 + 1;
        var pix = new byte[w * h * 4];
        for (int i = 0; i < w * h; i++)
        {
            byte a = i + start < data.Length ? data[start + i] : (byte)0;
            pix[i * 4] = a;
            pix[i * 4 + 1] = a;
            pix[i * 4 + 2] = a;
            pix[i * 4 + 3] = a;
        }
        var bmp = new WriteableBitmap(w, h, 96, 96, PixelFormats.Bgra32, null);
        bmp.WritePixels(new Int32Rect(0, 0, w, h), pix, w * 4, 0);
        bmp.Freeze();
        return bmp;
    }

    private void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        if (FontList.SelectedItem is not Face face) return;
        if (MessageBox.Show("Remover " + face.Label + " de C:\\sodos\\fonts?",
                "Desinstalar", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;
        try
        {
            File.Delete(face.Path);
        }
        catch (Exception ex)
        {
            Status.Text = ex.Message;
            return;
        }
        _ready = false;
        LoadFonts();
    }
}
