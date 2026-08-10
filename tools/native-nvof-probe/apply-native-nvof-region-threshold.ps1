$ErrorActionPreference = 'Stop'

$path = 'Source/NvidiaOpticalFlowDenseSynthesizer.cpp'
$text = [IO.File]::ReadAllText($path)
$old = @'
    const RegionGateParameters regionValues = {
        m_flowWidth, m_flowHeight, 8u, 3u,
    };
'@
$new = @'
    const RegionGateParameters regionValues = {
        // 18/49 (~36.7%) requires a genuinely dense catastrophic cluster.
        // The previous 8/49 threshold over-triggered on ordinary 23.976p
        // motion blur and effectively collapsed long stretches back to 24p.
        m_flowWidth, m_flowHeight, 18u, 3u,
    };
'@
if (-not $text.Contains($old)) {
    throw 'Regional gate parameter block was not found or already changed.'
}
$text = $text.Replace($old, $new)
[IO.File]::WriteAllText($path, $text)

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add -- $path
git commit -m 'Tighten regional NVOF frame rejection threshold'
git push origin HEAD:feature/native-nvof-interpolation
