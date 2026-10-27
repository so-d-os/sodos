Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;
public class TtfLic {
  public static string Read(string path) {
    byte[] b = File.ReadAllBytes(path);
    if (b.Length < 12) return "";
    int n = (b[4] << 8) | b[5];
    int nameOff = -1;
    for (int i = 0; i < n; i++) {
      int r = 12 + i * 16;
      if (r + 16 > b.Length) break;
      string tag = Encoding.ASCII.GetString(b, r, 4);
      int off = (b[r+8] << 24) | (b[r+9] << 16) | (b[r+10] << 8) | b[r+11];
      if (tag == "name") { nameOff = off; break; }
    }
    if (nameOff < 0 || nameOff + 6 > b.Length) return "";
    int count = (b[nameOff+2] << 8) | b[nameOff+3];
    int strOff = (b[nameOff+4] << 8) | b[nameOff+5];
    string copy = "", lic = "", fam = "";
    for (int i = 0; i < count; i++) {
      int r = nameOff + 6 + i * 12;
      if (r + 12 > b.Length) break;
      int plat = (b[r] << 8) | b[r+1];
      int id = (b[r+6] << 8) | b[r+7];
      int len = (b[r+8] << 8) | b[r+9];
      int o = (b[r+10] << 8) | b[r+11];
      int abs = nameOff + strOff + o;
      if (abs < 0 || abs + len > b.Length) continue;
      string s = (plat == 0 || plat == 3)
        ? Encoding.BigEndianUnicode.GetString(b, abs, len)
        : Encoding.ASCII.GetString(b, abs, len);
      s = s.Replace("\r", " ").Replace("\n", " ");
      if (id == 1 && fam.Length == 0) fam = s;
      if (id == 0 && copy.Length < s.Length) copy = s;
      if (id == 13 && lic.Length < s.Length) lic = s;
    }
    return fam + "\t" + copy + "\t" + lic;
  }
}
"@
Get-ChildItem 'C:\Windows\Fonts\*.ttf' | Sort-Object Name -Unique | ForEach-Object {
  try {
    $t = [TtfLic]::Read($_.FullName)
    if ($t -match 'public domain|Open Font|SIL |OFL|Apache|MIT License|freely|GNU |GPL|Unlicense') {
      $cut = $t
      if ($cut.Length -gt 240) { $cut = $cut.Substring(0, 240) }
      Write-Output ($_.Name + ' | ' + $cut)
    }
  } catch {}
}
