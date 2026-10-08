Attribute VB_Name = "modNes"
'Attribute VB_Name = "Module1"
Option Explicit

' --- Win32 API 선언 (생략 없이 기존대로 유지) ---
Public Declare Sub CopyMemory Lib "kernel32" Alias "RtlMoveMemory" (ByVal Destination As Long, ByVal Source As Long, ByVal Length As Long)
Public Declare Function timeGetTime Lib "winmm.dll" () As Long
Public Declare Function timeBeginPeriod Lib "winmm.dll" (ByVal uPeriod As Long) As Long
Public Declare Function timeEndPeriod Lib "winmm.dll" (ByVal uPeriod As Long) As Long
Public Declare Function Sleep Lib "kernel32" (ByVal dwMilliseconds As Long) As Long
Public Declare Function GetAsyncKeyState Lib "user32" (ByVal vKey As Long) As Integer

Public Declare Function CreateCompatibleDC Lib "gdi32" (ByVal hdc As Long) As Long
Public Declare Function DeleteDC Lib "gdi32" (ByVal hdc As Long) As Long
Public Declare Function DeleteObject Lib "gdi32" (ByVal hObject As Long) As Long
Public Declare Function SelectObject Lib "gdi32" (ByVal hdc As Long, ByVal hObject As Long) As Long
Public Declare Function StretchBlt Lib "gdi32" (ByVal hdcDest As Long, ByVal nXOriginDest As Long, ByVal nYOriginDest As Long, ByVal nWidthDest As Long, ByVal nHeightDest As Long, ByVal hdcSrc As Long, ByVal nXOriginSrc As Long, ByVal nYOriginSrc As Long, ByVal nWidthSrc As Long, ByVal nHeightSrc As Long, ByVal dwRop As Long) As Long
Public Declare Function SetStretchBltMode Lib "gdi32" (ByVal hdc As Long, ByVal nStretchMode As Long) As Long
Public Declare Function TextOut Lib "gdi32" Alias "TextOutA" (ByVal hdc As Long, ByVal x As Long, ByVal y As Long, ByVal lpString As String, ByVal nCount As Long) As Long
Public Declare Function SetBkMode Lib "gdi32" (ByVal hdc As Long, ByVal nBkMode As Long) As Long
Public Declare Function SetTextColor Lib "gdi32" (ByVal hdc As Long, ByVal crColor As Long) As Long

Public Const COLORONCOLOR As Long = 3
Public Const SRCCOPY As Long = &HCC0020
Public Const DIB_RGB_COLORS As Long = 0
Public Const TRANSPARENT As Long = 1

Public Type BITMAPINFOHEADER
    biSize As Long
    biWidth As Long
    biHeight As Long
    biPlanes As Integer
    biBitCount As Integer
    biCompression As Long
    biSizeImage As Long
    biXPelsPerMeter As Long
    biYPelsPerMeter As Long
    biClrUsed As Long
    biClrImportant As Long
End Type

Public Type BITMAPINFO
    bmiHeader As BITMAPINFOHEADER
    bmiColors As Long
End Type

Public Declare Function CreateDIBSection Lib "gdi32" (ByVal hdc As Long, pBitmapInfo As BITMAPINFO, ByVal un As Long, ByRef lplpVoid As Long, ByVal handle As Long, ByVal dw As Long) As Long

Public Type OPENFILENAME
    lStructSize As Long
    hwndOwner As Long
    hInstance As Long
    lpstrFilter As String
    lpstrCustomFilter As String
    nMaxCustFilter As Long
    nFilterIndex As Long
    lpstrFile As String
    nMaxFile As Long
    lpstrFileTitle As String
    nMaxFileTitle As Long
    lpstrInitialDir As String
    lpstrTitle As String
    flags As Long
    nFileOffset As Integer
    nFileExtension As Integer
    lpstrDefExt As String
    lCustData As Long
    lpfnHook As Long
    lpTemplateName As String
End Type

Public Declare Function BitBlt Lib "gdi32" (ByVal hDestDC As Long, ByVal x As Long, ByVal y As Long, ByVal nWidth As Long, ByVal nHeight As Long, ByVal hSrcDC As Long, ByVal xSrc As Long, ByVal ySrc As Long, ByVal dwRop As Long) As Long

Public Declare Function GetOpenFileName Lib "comdlg32.dll" Alias "GetOpenFileNameA" (pOpenfilename As OPENFILENAME) As Long

' ========================================================
' smolnes_sound.dll C 함수 선언
' ========================================================
' filepath 포인터 1개 (4바이트): @4
Public Declare Function NES_LoadROM Lib "smolnes_sound.dll" Alias "_NES_LoadROM@4" (ByVal filePath As String) As Long

' buttons Byte 1개 (스택 4바이트 정렬): @4
Public Declare Sub NES_SetInput Lib "smolnes_sound.dll" Alias "_NES_SetInput@4" (ByVal buttons As Byte)

' 인자 없음: @0
Public Declare Sub NES_RunFrame Lib "smolnes_sound.dll" Alias "_NES_RunFrame@0" ()

' 인자 없음: @0
Public Declare Function NES_GetBuffer Lib "smolnes_sound.dll" Alias "_NES_GetBuffer@0" () As Long

' 사운드 초기화 (인자 없음): @0
Public Declare Function NES_InitAudio Lib "smolnes_sound.dll" Alias "_NES_InitAudio@0" () As Long

' 사운드 해제 (인자 없음): @0
Public Declare Sub NES_CloseAudio Lib "smolnes_sound.dll" Alias "_NES_CloseAudio@0" ()


Public Function SelectNESRom(ByVal hwndOwner As Long) As String
    Dim ofn As OPENFILENAME
    Dim strFile As String
    
    strFile = String$(260, 0)
    With ofn
        .lStructSize = Len(ofn)
        .hwndOwner = hwndOwner
        .lpstrFilter = "NES ROM Files (*.nes)" & vbNullChar & "*.nes" & vbNullChar & "All Files (*.*)" & vbNullChar & "*.*" & vbNullChar
        .lpstrFile = strFile
        .nMaxFile = Len(strFile)
        .lpstrTitle = "NES ROM 파일 선택"
        .flags = &H80000 Or &H4 Or &H8
    End With
    
    If GetOpenFileName(ofn) <> 0 Then
        SelectNESRom = Left$(ofn.lpstrFile, InStr(ofn.lpstrFile, vbNullChar) - 1)
    Else
        SelectNESRom = ""
    End If
End Function

' 파일 경로에서 순수 파일명(확장자 제외) 추출
Public Function GetGameTitle(ByVal filePath As String) As String
    Dim pos1 As Long, pos2 As Long
    Dim fileName As String
    
    pos1 = InStrRev(filePath, "\")
    If pos1 > 0 Then
        fileName = Mid$(filePath, pos1 + 1)
    Else
        fileName = filePath
    End If
    
    pos2 = InStrRev(fileName, ".")
    If pos2 > 0 Then
        GetGameTitle = Left$(fileName, pos2 - 1)
    Else
        GetGameTitle = fileName
    End If
End Function

' iNES 헤더(Byte 6, 7)로부터 매퍼 번호 읽기
Public Function GetNESMapperNumber(ByVal filePath As String) As Long
    Dim fn As Integer
    Dim header(0 To 15) As Byte
    
    On Error GoTo ErrHandler
    fn = FreeFile
    Open filePath For Binary Access Read As #fn
    Get #fn, 1, header
    Close #fn
    
    ' "NES" 매직 넘버 검증
    If header(0) = &H4E And header(1) = &H45 And header(2) = &H53 And header(3) = &H1A Then
        ' (Byte 7의 상위 4비트) | (Byte 6의 상위 4비트)
        GetNESMapperNumber = (header(7) And &HF0) Or ((header(6) And &HF0) \ 16)
    Else
        GetNESMapperNumber = -1
    End If
    Exit Function

ErrHandler:
    If fn <> 0 Then Close #fn
    GetNESMapperNumber = -1
End Function
