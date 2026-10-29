# Contact sheet: tile PNGs (in name order) into one image.
#   contact.ps1 <glob> <out.png> [cols=4] [tileWidth=640]
param([string]$Glob, [string]$Out, [int]$Cols = 4, [int]$TileW = 640)
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem $Glob | Sort-Object Name
if (!$files) { "no files"; exit 1 }
$first = [System.Drawing.Image]::FromFile($files[0].FullName)
$tileH = [int]($TileW * $first.Height / $first.Width); $first.Dispose()
$rows = [Math]::Ceiling($files.Count / $Cols)
$bmp = New-Object System.Drawing.Bitmap ($Cols * $TileW), ($rows * $tileH)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = 'HighQualityBilinear'
$font = New-Object System.Drawing.Font 'Arial', 14
for ($i = 0; $i -lt $files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile($files[$i].FullName)
  $x = ($i % $Cols) * $TileW; $y = [Math]::Floor($i / $Cols) * $tileH
  $g.DrawImage($img, $x, $y, $TileW, $tileH); $img.Dispose()
  $g.DrawString("$i", $font, [System.Drawing.Brushes]::Yellow, $x + 4, $y + 4)
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); $Out
