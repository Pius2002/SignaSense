$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$voiceName = 'Microsoft David Desktop'
$voicePitch = '+18%'
$voiceRate = '-3%'
$outDir = 'D:\desktop\Glove\SmartStickESP32\voice_work'
$phrases = @{
  startup = 'Smart stick ready.'
  path_clear = 'Path ahead is clear.'
  no_echo = 'No reliable distance reading.'
  obstacle_ahead = 'Obstacle ahead.'
  point = 'point.'
  obstacle_150 = 'Obstacle ahead, about one hundred fifty centimeters. Slow down.'
  obstacle_100 = 'Obstacle ahead, about one hundred centimeters. Slow down.'
  obstacle_60 = 'Obstacle ahead, about sixty centimeters. Stop and scan.'
  obstacle_40 = 'Obstacle ahead, about forty centimeters. Stop.'
  obstacle_20 = 'Obstacle very close, less than twenty centimeters. Stop now.'
  scan_left = 'Stop. Scan left slowly.'
  scan_right = 'Now scan right slowly.'
  left_clearer = 'Left seems clearer. Move left carefully.'
  right_clearer = 'Right seems clearer. Move right carefully.'
  no_clear_side = 'No clear side. Stop and wait.'
  forward_carefully = 'Move forward carefully.'
}
foreach ($key in $phrases.Keys) {
  $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
  $s.Volume = 100
  $path = Join-Path $outDir ($key + '.wav')
  $s.SetOutputToWaveFile($path)
  $safePhrase = [System.Security.SecurityElement]::Escape($phrases[$key])
  $ssml = "<speak version='1.0' xml:lang='en-US'><voice name='$voiceName'><prosody pitch='$voicePitch' rate='$voiceRate'>$safePhrase</prosody></voice></speak>"
  $s.SpeakSsml($ssml)
  $s.SetOutputToNull()
  $s.Dispose()
}
