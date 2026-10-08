Attribute VB_Name = "modNes"
'Attribute VB_Name = "Module1"
Option Explicit

' ========================================================
' Win32 API 선언 (GDI, 메모리 복사, 고정밀 타이머, 키 입력)
' ========================================================
Public Declare Sub CopyMemory Lib "kernel32" Alias "RtlMoveMemory" ( _
    ByVal Destination As Long, _
    ByVal Source As Long, _
    ByVal Length As Long)

Public Declare Function timeGetTime Lib "winmm.dll" () As Long
Public Declare Function timeBeginPeriod Lib "winmm.dll" (ByVal uPeriod As Long) As Long
Public Declare Function timeEndPeriod Lib "winmm.dll" (ByVal uPeriod As Long) As Long
Public Declare Function Sleep Lib "kernel32" (ByVal dwMilliseconds As Long) As Long
Public Declare Function GetAsyncKeyState Lib "user32" (ByVal vKey As Long) As Integer

' DIB 및 GDI API
Public Declare Function CreateCompatibleDC Lib "gdi32" (ByVal hdc As Long) As Long
Public Declare Function DeleteDC Lib "gdi32" (ByVal hdc As Long) As Long
Public Declare Function DeleteObject Lib "gdi32" (ByVal hObject As Long) As Long
Public Declare Function SelectObject Lib "gdi32" (ByVal hdc As Long, ByVal hObject As Long) As Long
Public Declare Function BitBlt Lib "gdi32" ( _
    ByVal hDestDC As Long, ByVal x As Long, ByVal y As Long, _
    ByVal nWidth As Long, ByVal nHeight As Long, _
    ByVal hSrcDC As Long, ByVal xSrc As Long, ByVal ySrc As Long, _
    ByVal dwRop As Long) As Long

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

Public Declare Function CreateDIBSection Lib "gdi32" ( _
    ByVal hdc As Long, _
    pBitmapInfo As BITMAPINFO, _
    ByVal un As Long, _
    ByRef lplpVoid As Long, _
    ByVal handle As Long, _
    ByVal dw As Long) As Long

Public Const SRCCOPY As Long = &HCC0020
Public Const DIB_RGB_COLORS As Long = 0


' 2배 확대를 위한 StretchBlt 및 도트 유지 모드 설정
Public Declare Function StretchBlt Lib "gdi32" ( _
    ByVal hdcDest As Long, ByVal nXOriginDest As Long, ByVal nYOriginDest As Long, _
    ByVal nWidthDest As Long, ByVal nHeightDest As Long, _
    ByVal hdcSrc As Long, ByVal nXOriginSrc As Long, ByVal nYOriginSrc As Long, _
    ByVal nWidthSrc As Long, ByVal nHeightSrc As Long, ByVal dwRop As Long) As Long

Public Declare Function SetStretchBltMode Lib "gdi32" (ByVal hdc As Long, ByVal nStretchMode As Long) As Long
Public Const COLORONCOLOR = 3


' --- Win32 순수 파일 열기 대화상자 (OCX 불필요) ---
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

Public Declare Function GetOpenFileName Lib "comdlg32.dll" Alias "GetOpenFileNameA" (pOpenfilename As OPENFILENAME) As Long

' --- 텍스트 오버레이 출력용 Win32 GDI API ---
Public Declare Function TextOut Lib "gdi32" Alias "TextOutA" ( _
    ByVal hdc As Long, ByVal x As Long, ByVal y As Long, _
    ByVal lpString As String, ByVal nCount As Long) As Long

Public Declare Function SetBkMode Lib "gdi32" (ByVal hdc As Long, ByVal nBkMode As Long) As Long
Public Declare Function SetTextColor Lib "gdi32" (ByVal hdc As Long, ByVal crColor As Long) As Long

Public Const TRANSPARENT As Long = 1

' ========================================================
' smolnes.dll C 함수 선언
' ========================================================

' 포인터 1개(4바이트): @4
Public Declare Function NES_LoadROM Lib "smolnes.dll" Alias "_NES_LoadROM@4" (ByVal filepath As String) As Long

' Byte 1개(스택 4바이트 정렬): @4
Public Declare Sub NES_SetInput Lib "smolnes.dll" Alias "_NES_SetInput@4" (ByVal buttons As Byte)

' 인자 없음(0바이트): @0
Public Declare Sub NES_RunFrame Lib "smolnes.dll" Alias "_NES_RunFrame@0" ()

' 인자 없음(0바이트): @0
Public Declare Function NES_GetBuffer Lib "smolnes.dll" Alias "_NES_GetBuffer@0" () As Long

' ROM 선택 함수
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
        .flags = &H80000 Or &H4 Or &H8 ' OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR
    End With
    
    If GetOpenFileName(ofn) <> 0 Then
        SelectNESRom = Left$(ofn.lpstrFile, InStr(ofn.lpstrFile, vbNullChar) - 1)
    Else
        SelectNESRom = ""
    End If
End Function
