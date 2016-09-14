/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "_stream.h"
#include "stream.h"

#include "dbcs.h"
#include "handle.h"
#include "misc.h"
#include "output.h"
#include "cursor.h"

#pragma hdrstop

#define LINE_INPUT_BUFFER_SIZE (256 * sizeof(WCHAR))

#define IS_JPN_1BYTE_KATAKANA(c)   ((c) >= 0xa1 && (c) <= 0xdf)

// Convert real Windows NT modifier bit into bizarre Console bits
#define EITHER_CTRL_PRESSED (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)
#define EITHER_ALT_PRESSED (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)
#define MOD_PRESSED (SHIFT_PRESSED | EITHER_CTRL_PRESSED | EITHER_ALT_PRESSED)

typedef struct _RAW_READ_DATA
{
    PINPUT_INFORMATION InputInfo;
    ULONG BufferSize;
    _Field_size_(BufferSize) PWCHAR BufPtr;
    PCONSOLE_PROCESS_HANDLE ProcessData;
    HANDLE HandleIndex;
} RAW_READ_DATA, *PRAW_READ_DATA;

DWORD ConsKbdState[] = {
    0,
    SHIFT_PRESSED,
    EITHER_CTRL_PRESSED,
    SHIFT_PRESSED | EITHER_CTRL_PRESSED,
    EITHER_ALT_PRESSED,
    SHIFT_PRESSED | EITHER_ALT_PRESSED,
    EITHER_CTRL_PRESSED | EITHER_ALT_PRESSED,
    SHIFT_PRESSED | EITHER_CTRL_PRESSED | EITHER_ALT_PRESSED
};

#define KEYEVENTSTATE_EQUAL_WINMODS(Event, WinMods)\
    ((Event.Event.KeyEvent.dwControlKeyState & ConsKbdState[WinMods]) && \
    !(Event.Event.KeyEvent.dwControlKeyState & MOD_PRESSED & ~ConsKbdState[WinMods]))

// Routine Description:
// - This routine is used in stream input.  It gets input and filters it for unicode characters.
// Arguments:
// - InputInfo - Pointer to input buffer information.
// - Char - Unicode char input.
// - Wait - TRUE if the routine shouldn't wait for input.
// - Console - Pointer to console buffer information.
// - HandleData - Pointer to handle data structure.
// - Message - csr api message.
// - WaitRoutine - Routine to call when wait is woken up.
// - WaitParameter - Parameter to pass to wait routine.
// - WaitParameterLength - Length of wait parameter.
// - WaitBlockExists - TRUE if wait block has already been created.
// - CommandLineEditingKeys - if present, arrow keys will be returned. on output, if TRUE, Char contains virtual key code for arrow key.
// - CommandLinePopupKeys - if present, arrow keys will be returned. on output, if TRUE, Char contains virtual key code for arrow key.
// Return Value:
NTSTATUS GetChar(_In_ PINPUT_INFORMATION pInputInfo,
                 _Out_ PWCHAR pwchOut,
                 _In_ const BOOL fWait,
                 _In_opt_ PCONSOLE_HANDLE_DATA pHandleData,
                 _In_opt_ PCONSOLE_API_MSG pConsoleMessage,
                 _In_opt_ CONSOLE_WAIT_ROUTINE pWaitRoutine,
                 _In_opt_ PVOID pvWaitParameter,
                 _In_opt_ ULONG ulWaitParameterLength,
                 _In_opt_ BOOLEAN fWaitBlockExists,
                 _Out_opt_ PBOOLEAN pfCommandLineEditingKeys,
                 _Out_opt_ PBOOLEAN pfCommandLinePopupKeys,
                 _Out_opt_ PBOOLEAN pfEnableScrollMode,
                 _Out_opt_ PDWORD pdwKeyState)
{
    if (nullptr != pfCommandLineEditingKeys)
    {
        *pfCommandLineEditingKeys = FALSE;
    }

    if (nullptr != pfCommandLinePopupKeys)
    {
        *pfCommandLinePopupKeys = FALSE;
    }

    if (nullptr != pfEnableScrollMode)
    {
        *pfEnableScrollMode = FALSE;
    }

    if (nullptr != pdwKeyState)
    {
        *pdwKeyState = 0;
    }

    NTSTATUS Status;
    for (;;)
    {
        INPUT_RECORD Event;
        ULONG NumRead = 1;
        Status = ReadInputBuffer(pInputInfo,
                                 &Event,
                                 &NumRead,
                                 FALSE, /*Peek*/
                                 fWait,
                                 TRUE, /*StreamRead*/
                                 pHandleData,
                                 pConsoleMessage,
                                 pWaitRoutine,
                                 pvWaitParameter,
                                 ulWaitParameterLength,
                                 fWaitBlockExists,
                                 TRUE); /*Unicode*/
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        if (NumRead == 0)
        {
            ASSERT(!fWait);

            return STATUS_UNSUCCESSFUL;
        }

        if (Event.EventType == KEY_EVENT)
        {
            BOOL fCommandLineEditKey;

            if (nullptr != pfCommandLineEditingKeys)
            {
                fCommandLineEditKey = IsCommandLineEditingKey(&Event.Event.KeyEvent);
            }
            else if (nullptr != pfCommandLinePopupKeys)
            {
                fCommandLineEditKey = IsCommandLinePopupKey(&Event.Event.KeyEvent);
            }
            else
            {
                fCommandLineEditKey = FALSE;
            }

            // Always return keystate if caller asked for it.
            if (nullptr != pdwKeyState)
            {
                *pdwKeyState = Event.Event.KeyEvent.dwControlKeyState;
            }

            if (Event.Event.KeyEvent.uChar.UnicodeChar != 0 && !fCommandLineEditKey)
            {
                // chars that are generated using alt + numpad
                if (!Event.Event.KeyEvent.bKeyDown && Event.Event.KeyEvent.wVirtualKeyCode == VK_MENU)
                {
                    if (Event.Event.KeyEvent.dwControlKeyState & ALTNUMPAD_BIT)
                    {
                        if (HIBYTE(Event.Event.KeyEvent.uChar.UnicodeChar))
                        {
                            char chT[2] = {
                                HIBYTE(Event.Event.KeyEvent.uChar.UnicodeChar),
                                LOBYTE(Event.Event.KeyEvent.uChar.UnicodeChar),
                            };
                            *pwchOut = CharToWchar(chT, 2);
                        }
                        else
                        {
                            // Because USER doesn't know our codepage, it gives us the raw OEM char and we convert it to a Unicode character.
                            char chT = LOBYTE(Event.Event.KeyEvent.uChar.UnicodeChar);
                            *pwchOut = CharToWchar(&chT, 1);
                        }
                    }
                    else
                    {
                        *pwchOut = Event.Event.KeyEvent.uChar.UnicodeChar;
                    }
                    return STATUS_SUCCESS;
                }
                // Ignore Escape and Newline chars
                else if (Event.Event.KeyEvent.bKeyDown &&   
                         //if we're in VT Input, we want everything. Else, ignore Escape and newlines
                         ( IsFlagSet(pInputInfo->InputMode, ENABLE_VIRTUAL_TERMINAL_INPUT) ||
                           (Event.Event.KeyEvent.wVirtualKeyCode != VK_ESCAPE && Event.Event.KeyEvent.uChar.UnicodeChar != 0x0A) ) )
                            
                {
                    *pwchOut = Event.Event.KeyEvent.uChar.UnicodeChar;
                    return STATUS_SUCCESS;
                }

            }

            if (Event.Event.KeyEvent.bKeyDown)
            {
                SHORT sTmp;

                if ((nullptr != pfCommandLineEditingKeys) && fCommandLineEditKey)
                {
                    *pfCommandLineEditingKeys = TRUE;
                    *pwchOut = (WCHAR)Event.Event.KeyEvent.wVirtualKeyCode;
                    return STATUS_SUCCESS;
                }
                else if ((nullptr != pfCommandLinePopupKeys) && fCommandLineEditKey)
                {
                    *pfCommandLinePopupKeys = TRUE;
                    *pwchOut = (CHAR) Event.Event.KeyEvent.wVirtualKeyCode;
                    return STATUS_SUCCESS;
                }

                sTmp = VkKeyScanW(0);

                #pragma prefast(suppress:26019, "Legacy. PREfast has a theoretical VK which jumps the table. Leaving.")
                if ((LOBYTE(sTmp) == Event.Event.KeyEvent.wVirtualKeyCode) && KEYEVENTSTATE_EQUAL_WINMODS(Event, HIBYTE(sTmp)))
                {
                    // This really is the character 0x0000
                    *pwchOut = Event.Event.KeyEvent.uChar.UnicodeChar;
                    return STATUS_SUCCESS;
                }
            }
        }
    }
}

// Routine Description:
// - This routine is called when a ReadConsole or ReadFile request is about to be completed.
// - It sets the number of bytes written as the information to be written with the completion status and,
//   if CTRL+Z processing is enabled and a CTRL+Z is detected, switches the number of bytes read to zero.
// Arguments:
// - Message - Supplies the message that is about to be completed.
// Return Value:
// - <none>
VOID PrepareReadConsoleCompletion(_Inout_ PCONSOLE_API_MSG Message)
{
    PCONSOLE_READCONSOLE_MSG const a = &Message->u.consoleMsgL1.ReadConsole;

    if (a->ProcessControlZ != FALSE &&
        a->NumBytes > 0 &&
        Message->State.OutputBuffer != nullptr &&
        *(PUCHAR)Message->State.OutputBuffer == 0x1a)
    {
        a->NumBytes = 0;
    }

    SetReplyInformation(Message, a->NumBytes);
}

// Routine Description:
// - This routine is called to complete a raw read that blocked in ReadInputBuffer.
// - The context of the read was saved in the RawReadData structure.
// - This routine is called when events have been written to the input buffer.
// - It is called in the context of the writing thread.
// - It will be called at most once per read.?
// Arguments:
// - WaitQueue - pointer to queue containing wait block
// - WaitReplyMessage - Pointer to reply message to return to dll whenread is completed.
// - RawReadData - pointer to data saved in ReadChars
// - SatisfyParameter - Flags.
// - ThreadDying - Indicates whether the thread (and process) is exiting.
// Return Value:
BOOL RawReadWaitRoutine(_In_ PLIST_ENTRY /*WaitQueue*/, 
                        _In_ PCONSOLE_API_MSG pWaitReplyMessage,
                        _In_ PVOID pvWaitParameter,
                        _In_ PVOID pvSatisfyParameter,
                        _In_ BOOL fThreadDying)
{
    PCONSOLE_READCONSOLE_MSG const a = &pWaitReplyMessage->u.consoleMsgL1.ReadConsole;
    PRAW_READ_DATA const RawReadData = (PRAW_READ_DATA)pvWaitParameter;

    PCONSOLE_HANDLE_DATA HandleData;
    NTSTATUS Status = DereferenceIoHandleNoCheck(RawReadData->HandleIndex, &HandleData);
    if (!NT_SUCCESS(Status))
    {
        return TRUE;
    }

    if ((ULONG_PTR)pvSatisfyParameter & CONSOLE_CTRL_C_SEEN)
    {
        return FALSE;
    }

    // This routine should be called by a thread owning the same lock on the same console as we're reading from.
    a->NumBytes = 0;
    DWORD NumBytes = 0;

    PWCHAR lpBuffer;
    BOOLEAN RetVal = TRUE;
    BOOL fAddDbcsLead = FALSE;
    bool fSkipFinally = false;
    __try
    {
#ifdef DBG
        HandleData->pClientInput->LockReadCount();
        ASSERT(HandleData->pClientInput->GetReadCount() > 0);
        HandleData->pClientInput->UnlockReadCount();
#endif
        HandleData->pClientInput->DecrementReadCount();

        // If a ctrl-c is seen, don't terminate read. If ctrl-break is seen, terminate read.
        if ((ULONG_PTR)pvSatisfyParameter & CONSOLE_CTRL_BREAK_SEEN)
        {
            SetReplyStatus(pWaitReplyMessage, STATUS_ALERTED);
            __leave;
        }

        // See if we were called because the thread that owns this wait block is exiting.
        if (fThreadDying)
        {
            Status = STATUS_THREAD_IS_TERMINATING;
            __leave;
        }

        // We must see if we were woken up because the handle is being
        // closed. If so, we decrement the read count. If it goes to zero,
        // we wake up the close thread. Otherwise, we wake up any other
        // thread waiting for data.

        if (HandleData->pClientInput->InputHandleFlags & HANDLE_CLOSING)
        {
            Status = STATUS_ALERTED;
            __leave;
        }

        // If we get to here, this routine was called either by the input
        // thread or a write routine. Both of these callers grab the current
        // console lock.

        // This routine should be called by a thread owning the same lock on
        // the same console as we're reading from.

        ASSERT(g_ciConsoleInformation.IsConsoleLocked());

        lpBuffer = RawReadData->BufPtr;

        // This call to GetChar may block.
        if (!a->Unicode)
        {
            if (GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
            {
                fAddDbcsLead = TRUE;
                *lpBuffer = GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                RawReadData->BufferSize -= sizeof(WCHAR);
                ZeroMemory(&GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                Status = STATUS_SUCCESS;
                if (RawReadData->BufferSize == 0)
                {
                    a->NumBytes = 1;
                    RetVal = FALSE;
                    fSkipFinally = true;
                    __leave;
                }
            }
            else
            {
                Status = GetChar(RawReadData->InputInfo,
                                 lpBuffer,
                                 TRUE,
                                 HandleData,
                                 pWaitReplyMessage,
                                 RawReadWaitRoutine,
                                 RawReadData,
                                 sizeof(*RawReadData),
                                 TRUE,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr);
            }
        }
        else
        {
            Status = GetChar(RawReadData->InputInfo,
                             lpBuffer,
                             TRUE,
                             HandleData,
                             pWaitReplyMessage,
                             RawReadWaitRoutine,
                             RawReadData,
                             sizeof(*RawReadData),
                             TRUE,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr);
        }

        if (!NT_SUCCESS(Status))
        {
            if (Status == CONSOLE_STATUS_WAIT)
            {
                RetVal = FALSE;
            }
            __leave;
        }
        IsCharFullWidth(*lpBuffer) ? NumBytes += 2 : NumBytes++;
        lpBuffer++;
        a->NumBytes += sizeof(WCHAR);
        while (a->NumBytes < RawReadData->BufferSize)
        {
            // This call to GetChar won't block.
            Status = GetChar(RawReadData->InputInfo, lpBuffer, FALSE, nullptr, nullptr, nullptr, nullptr, 0, TRUE, nullptr, nullptr, nullptr, nullptr);
            if (!NT_SUCCESS(Status))
            {
                Status = STATUS_SUCCESS;
                break;
            }
            IsCharFullWidth(*lpBuffer) ? NumBytes += 2 : NumBytes++;
            lpBuffer++;
            a->NumBytes += sizeof(WCHAR);
        }
    } __finally
    {
        // If the read was completed (status != wait), free the raw read data.
        if (Status != CONSOLE_STATUS_WAIT && !fSkipFinally)
        {
            if (!a->Unicode)
            {
                PCHAR TransBuffer;

                // It's ansi, so translate the string.
                TransBuffer = (PCHAR) new BYTE[NumBytes];
                if (TransBuffer == nullptr)
                {
                    RetVal = TRUE;
                    goto EndFinally;
                }

                lpBuffer = RawReadData->BufPtr;

                a->NumBytes = TranslateUnicodeToOem(lpBuffer, a->NumBytes / sizeof(WCHAR), TransBuffer, NumBytes, &GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte);

                memmove(lpBuffer, TransBuffer, a->NumBytes);
                if (fAddDbcsLead)
                {
                    a->NumBytes++;
                }

                delete[] TransBuffer;
            }

            SetReplyStatus(pWaitReplyMessage, Status);
            PrepareReadConsoleCompletion(pWaitReplyMessage);
        }
EndFinally:;
    }

    return RetVal;
}

// Routine Description:
// - This routine returns the total number of screen spaces the characters up to the specified character take up.
ULONG RetrieveTotalNumberOfSpaces(_In_ const SHORT sOriginalCursorPositionX,
                                  _In_reads_(ulCurrentPosition) const WCHAR * const pwchBuffer,
                                  _In_ ULONG ulCurrentPosition)
{
    SHORT XPosition = sOriginalCursorPositionX;
    ULONG NumSpaces = 0;

    for (ULONG i = 0; i < ulCurrentPosition; i++)
    {
        WCHAR const Char = pwchBuffer[i];

        ULONG NumSpacesForChar;
        if (Char == UNICODE_TAB)
        {
            NumSpacesForChar = NUMBER_OF_SPACES_IN_TAB(XPosition);
        }
        else if (IS_CONTROL_CHAR(Char))
        {
            NumSpacesForChar = 2;
        }
        else if (IsCharFullWidth(Char))
        {
            NumSpacesForChar = 2;
        }
        else
        {
            NumSpacesForChar = 1;
        }
        XPosition = (SHORT)(XPosition + NumSpacesForChar);
        NumSpaces += NumSpacesForChar;
    }

    return NumSpaces;
}

// Routine Description:
// - This routine returns the number of screen spaces the specified character takes up.
ULONG RetrieveNumberOfSpaces(_In_ SHORT sOriginalCursorPositionX,
                             _In_reads_(ulCurrentPosition + 1) const WCHAR * const pwchBuffer,
                             _In_ ULONG ulCurrentPosition)
{
    WCHAR Char = pwchBuffer[ulCurrentPosition];
    if (Char == UNICODE_TAB)
    {
        ULONG NumSpaces = 0;
        SHORT XPosition = sOriginalCursorPositionX;

        for (ULONG i = 0; i <= ulCurrentPosition; i++)
        {
            Char = pwchBuffer[i];
            if (Char == UNICODE_TAB)
            {
                NumSpaces = NUMBER_OF_SPACES_IN_TAB(XPosition);
            }
            else if (IS_CONTROL_CHAR(Char))
            {
                NumSpaces = 2;
            }
            else if (IsCharFullWidth(Char))
            {
                NumSpaces = 2;
            }
            else
            {
                NumSpaces = 1;
            }
            XPosition = (SHORT)(XPosition + NumSpaces);
        }

        return NumSpaces;
    }
    else if (IS_CONTROL_CHAR(Char))
    {
        return 2;
    }
    else if (IsCharFullWidth(Char))
    {
        return 2;
    }
    else
    {
        return 1;
    }
}

// Return Value:
// - TRUE if read is completed
BOOL ProcessCookedReadInput(_In_ PCOOKED_READ_DATA pCookedReadData, _In_ WCHAR wch, _In_ const DWORD dwKeyState, _Out_ PNTSTATUS pStatus)
{
    DWORD NumSpaces = 0;
    SHORT ScrollY = 0;
    ULONG NumToWrite;
    WCHAR wchOrig = wch;
    BOOL fStartFromDelim;

    *pStatus = STATUS_SUCCESS;
    if (pCookedReadData->BytesRead >= (pCookedReadData->BufferSize - (2 * sizeof(WCHAR))) && wch != UNICODE_CARRIAGERETURN && wch != UNICODE_BACKSPACE)
    {
        return FALSE;
    }

    if (pCookedReadData->CtrlWakeupMask != 0 && wch < L' ' && (pCookedReadData->CtrlWakeupMask & (1 << wch)))
    {
        *pCookedReadData->BufPtr = wch;
        pCookedReadData->BytesRead += sizeof(WCHAR);
        pCookedReadData->BufPtr += 1;
        pCookedReadData->CurrentPosition += 1;
        pCookedReadData->ControlKeyState = dwKeyState;
        return TRUE;
    }

    if (wch == EXTKEY_ERASE_PREV_WORD)
    {
        wch = UNICODE_BACKSPACE;
    }

    if (AT_EOL(pCookedReadData))
    {
        // If at end of line, processing is relatively simple. Just store the character and write it to the screen.
        if (wch == UNICODE_BACKSPACE2)
        {
            wch = UNICODE_BACKSPACE;
        }

        if (wch != UNICODE_BACKSPACE || pCookedReadData->BufPtr != pCookedReadData->BackupLimit)
        {
            fStartFromDelim = g_ciConsoleInformation.GetExtendedEditKey() && IS_WORD_DELIM(pCookedReadData->BufPtr[-1]);

eol_repeat:
            if (pCookedReadData->Echo)
            {
                NumToWrite = sizeof(WCHAR);
                *pStatus = WriteCharsLegacy(pCookedReadData->ScreenInfo,
                                            pCookedReadData->BackupLimit,
                                            pCookedReadData->BufPtr,
                                            &wch,
                                            &NumToWrite,
                                            &NumSpaces,
                                            pCookedReadData->OriginalCursorPosition.X,
                                            WC_DESTRUCTIVE_BACKSPACE | WC_KEEP_CURSOR_VISIBLE | WC_ECHO,
                                            &ScrollY);
                if (NT_SUCCESS(*pStatus))
                {
                    pCookedReadData->OriginalCursorPosition.Y += ScrollY;
                }
                else
                {
                    RIPMSG1(RIP_WARNING, "WriteCharsLegacy failed %x", *pStatus);
                }
            }

            pCookedReadData->NumberOfVisibleChars += NumSpaces;
            if (wch == UNICODE_BACKSPACE && pCookedReadData->Processed)
            {
                pCookedReadData->BytesRead -= sizeof(WCHAR);
#pragma prefast(suppress:__WARNING_POTENTIAL_BUFFER_OVERFLOW_HIGH_PRIORITY, "This access is fine")
                *pCookedReadData->BufPtr = (WCHAR)' ';
                pCookedReadData->BufPtr -= 1;
                pCookedReadData->CurrentPosition -= 1;

                // Repeat until it hits the word boundary
                if (g_ciConsoleInformation.GetExtendedEditKey() &&
                    wchOrig == EXTKEY_ERASE_PREV_WORD &&
                    pCookedReadData->BufPtr != pCookedReadData->BackupLimit &&
                    fStartFromDelim ^ !IS_WORD_DELIM(pCookedReadData->BufPtr[-1]))
                {
                    goto eol_repeat;
                }
            }
            else
            {
                *pCookedReadData->BufPtr = wch;
                pCookedReadData->BytesRead += sizeof(WCHAR);
                pCookedReadData->BufPtr += 1;
                pCookedReadData->CurrentPosition += 1;
            }
        }
    }
    else
    {
        BOOL CallWrite = TRUE;

        // processing in the middle of the line is more complex:

        // calculate new cursor position
        // store new char
        // clear the current command line from the screen
        // write the new command line to the screen
        // update the cursor position

        if (wch == UNICODE_BACKSPACE && pCookedReadData->Processed)
        {
            // for backspace, use writechars to calculate the new cursor position.
            // this call also sets the cursor to the right position for the
            // second call to writechars.

            if (pCookedReadData->BufPtr != pCookedReadData->BackupLimit)
            {

                fStartFromDelim = g_ciConsoleInformation.GetExtendedEditKey() && IS_WORD_DELIM(pCookedReadData->BufPtr[-1]);

bs_repeat:
                // we call writechar here so that cursor position gets updated
                // correctly.  we also call it later if we're not at eol so
                // that the remainder of the string can be updated correctly.

                if (pCookedReadData->Echo)
                {
                    NumToWrite = sizeof(WCHAR);
                    *pStatus = WriteCharsLegacy(pCookedReadData->ScreenInfo,
                                                pCookedReadData->BackupLimit,
                                                pCookedReadData->BufPtr,
                                                &wch,
                                                &NumToWrite,
                                                nullptr,
                                                pCookedReadData->OriginalCursorPosition.X,
                                                WC_DESTRUCTIVE_BACKSPACE | WC_KEEP_CURSOR_VISIBLE | WC_ECHO,
                                                nullptr);
                    if (!NT_SUCCESS(*pStatus))
                    {
                        RIPMSG1(RIP_WARNING, "WriteCharsLegacy failed %x", *pStatus);
                    }
                }
                pCookedReadData->BytesRead -= sizeof(WCHAR);
                pCookedReadData->BufPtr -= 1;
                pCookedReadData->CurrentPosition -= 1;
                memmove(pCookedReadData->BufPtr,
                        pCookedReadData->BufPtr + 1,
                        pCookedReadData->BytesRead - (pCookedReadData->CurrentPosition * sizeof(WCHAR)));
                {
                    PWCHAR buf = (PWCHAR)((PBYTE) pCookedReadData->BackupLimit + pCookedReadData->BytesRead);
                    *buf = (WCHAR)' ';
                }
                NumSpaces = 0;

                // Repeat until it hits the word boundary
                if (g_ciConsoleInformation.GetExtendedEditKey() &&
                    wchOrig == EXTKEY_ERASE_PREV_WORD &&
                    pCookedReadData->BufPtr != pCookedReadData->BackupLimit &&
                    fStartFromDelim ^ !IS_WORD_DELIM(pCookedReadData->BufPtr[-1]))
                {
                    goto bs_repeat;
                }
            }
            else
            {
                CallWrite = FALSE;
            }
        }
        else
        {
            // store the char
            if (wch == UNICODE_CARRIAGERETURN)
            {
                pCookedReadData->BufPtr = (PWCHAR)((PBYTE) pCookedReadData->BackupLimit + pCookedReadData->BytesRead);
                *pCookedReadData->BufPtr = wch;
                pCookedReadData->BufPtr += 1;
                pCookedReadData->BytesRead += sizeof(WCHAR);
                pCookedReadData->CurrentPosition += 1;
            }
            else
            {
                BOOL fBisect = FALSE;

                if (pCookedReadData->Echo)
                {
                    if (CheckBisectProcessW(pCookedReadData->ScreenInfo,
                                            pCookedReadData->BackupLimit,
                                            pCookedReadData->CurrentPosition + 1,
                                            pCookedReadData->ScreenInfo->ScreenBufferSize.X - pCookedReadData->OriginalCursorPosition.X,
                                            pCookedReadData->OriginalCursorPosition.X,
                                            TRUE))
                    {
                        fBisect = TRUE;
                    }
                }

                if (pCookedReadData->InsertMode)
                {
                    memmove(pCookedReadData->BufPtr + 1,
                            pCookedReadData->BufPtr,
                            pCookedReadData->BytesRead - (pCookedReadData->CurrentPosition * sizeof(WCHAR)));
                    pCookedReadData->BytesRead += sizeof(WCHAR);
                }
                *pCookedReadData->BufPtr = wch;
                pCookedReadData->BufPtr += 1;
                pCookedReadData->CurrentPosition += 1;

                // calculate new cursor position
                if (pCookedReadData->Echo)
                {
                    NumSpaces = RetrieveNumberOfSpaces(pCookedReadData->OriginalCursorPosition.X,
                                                       pCookedReadData->BackupLimit,
                                                       pCookedReadData->CurrentPosition - 1);
                    if (NumSpaces > 0 && fBisect)
                        NumSpaces--;
                }
            }
        }

        if (pCookedReadData->Echo && CallWrite)
        {
            COORD CursorPosition;

            // save cursor position
            CursorPosition = pCookedReadData->ScreenInfo->TextInfo->GetCursor()->GetPosition();
            CursorPosition.X = (SHORT)(CursorPosition.X + NumSpaces);

            // clear the current command line from the screen
#pragma prefast(suppress:__WARNING_BUFFER_OVERFLOW, "Not sure why prefast doesn't like this call.")
            DeleteCommandLine(pCookedReadData, FALSE);

            // write the new command line to the screen
            NumToWrite = pCookedReadData->BytesRead;

            DWORD dwFlags = WC_DESTRUCTIVE_BACKSPACE | WC_ECHO;
            if (wch == UNICODE_CARRIAGERETURN)
            {
                dwFlags |= WC_KEEP_CURSOR_VISIBLE;
            }
            *pStatus = WriteCharsLegacy(pCookedReadData->ScreenInfo,
                                        pCookedReadData->BackupLimit,
                                        pCookedReadData->BackupLimit,
                                        pCookedReadData->BackupLimit,
                                        &NumToWrite,
                                        &pCookedReadData->NumberOfVisibleChars,
                                        pCookedReadData->OriginalCursorPosition.X,
                                        dwFlags,
                                        &ScrollY);
            if (!NT_SUCCESS(*pStatus))
            {
                RIPMSG1(RIP_WARNING, "WriteCharsLegacy failed 0x%x", *pStatus);
                pCookedReadData->BytesRead = 0;
                return TRUE;
            }

            // update cursor position
            if (wch != UNICODE_CARRIAGERETURN)
            {
                if (CheckBisectProcessW(pCookedReadData->ScreenInfo,
                                        pCookedReadData->BackupLimit,
                                        pCookedReadData->CurrentPosition + 1,
                                        pCookedReadData->ScreenInfo->ScreenBufferSize.X - pCookedReadData->OriginalCursorPosition.X,
                                        pCookedReadData->OriginalCursorPosition.X, TRUE))
                {
                    if (CursorPosition.X == (pCookedReadData->ScreenInfo->ScreenBufferSize.X - 1))
                    {
                        CursorPosition.X++;
                    }
                }

                // adjust cursor position for WriteChars
                pCookedReadData->OriginalCursorPosition.Y += ScrollY;
                CursorPosition.Y += ScrollY;
                *pStatus = AdjustCursorPosition(pCookedReadData->ScreenInfo, CursorPosition, TRUE, nullptr);
                ASSERT(NT_SUCCESS(*pStatus));
                if (!NT_SUCCESS(*pStatus))
                {
                    pCookedReadData->BytesRead = 0;
                    return TRUE;
                }
            }
        }
    }

    // in cooked mode, enter (carriage return) is converted to
    // carriage return linefeed (0xda).  carriage return is always
    // stored at the end of the buffer.
    if (wch == UNICODE_CARRIAGERETURN)
    {
        if (pCookedReadData->Processed)
        {
            if (pCookedReadData->BytesRead < pCookedReadData->BufferSize)
            {
                *pCookedReadData->BufPtr = UNICODE_LINEFEED;
                if (pCookedReadData->Echo)
                {
                    NumToWrite = sizeof(WCHAR);
                    *pStatus = WriteCharsLegacy(pCookedReadData->ScreenInfo,
                                                pCookedReadData->BackupLimit,
                                                pCookedReadData->BufPtr,
                                                pCookedReadData->BufPtr,
                                                &NumToWrite,
                                                nullptr,
                                                pCookedReadData->OriginalCursorPosition.X,
                                                WC_DESTRUCTIVE_BACKSPACE | WC_KEEP_CURSOR_VISIBLE | WC_ECHO,
                                                nullptr);
                    if (!NT_SUCCESS(*pStatus))
                    {
                        RIPMSG1(RIP_WARNING, "WriteCharsLegacy failed 0x%x", *pStatus);
                    }
                }
                pCookedReadData->BytesRead += sizeof(WCHAR);
                pCookedReadData->BufPtr++;
                pCookedReadData->CurrentPosition += 1;
            }
        }
        // reset the cursor back to 25% if necessary
        if (pCookedReadData->Line)
        {
            if (pCookedReadData->InsertMode != g_ciConsoleInformation.GetInsertMode())
            {
                // Make cursor small.
                ProcessCommandLine(pCookedReadData, VK_INSERT, 0, nullptr, FALSE);
            }

            *pStatus = STATUS_SUCCESS;
            return TRUE;
        }
    }

    return FALSE;
}

NTSTATUS CookedRead(_In_ PCOOKED_READ_DATA pCookedReadData, _In_ PCONSOLE_API_MSG pWaitReplyMessage, _In_ const BOOLEAN fWaitRoutine)
{
    PCONSOLE_HANDLE_DATA HandleData;
    NTSTATUS Status = DereferenceIoHandleNoCheck(pCookedReadData->HandleIndex, &HandleData);
    if (!NT_SUCCESS(Status))
    {
        pCookedReadData->BytesRead = 0;
        delete[] pCookedReadData->BackupLimit;
        return Status;
    }

    PCONSOLE_READCONSOLE_MSG const a = &pWaitReplyMessage->u.consoleMsgL1.ReadConsole;

    WCHAR Char;
    BOOLEAN CommandLineEditingKeys, EnableScrollMode;
    DWORD KeyState;
    DWORD NumBytes = 0;
    ULONG NumToWrite;
    BOOL fAddDbcsLead = FALSE;
    while (pCookedReadData->BytesRead < pCookedReadData->BufferSize)
    {
        // This call to GetChar may block.
        Status = GetChar(pCookedReadData->InputInfo,
                         &Char,
                         TRUE,
                         HandleData,
                         pWaitReplyMessage,
                         (CONSOLE_WAIT_ROUTINE)CookedReadWaitRoutine,
                         pCookedReadData,
                         sizeof(*pCookedReadData),
                         fWaitRoutine,
                         &CommandLineEditingKeys,
                         nullptr,
                         &EnableScrollMode,
                         &KeyState);
        if (!NT_SUCCESS(Status))
        {
            if (Status != CONSOLE_STATUS_WAIT)
            {
                pCookedReadData->BytesRead = 0;
            }
            break;
        }

        // we should probably set these up in GetChars, but we set them
        // up here because the debugger is multi-threaded and calls
        // read before outputting the prompt.

        if (pCookedReadData->OriginalCursorPosition.X == -1)
        {
            pCookedReadData->OriginalCursorPosition = pCookedReadData->ScreenInfo->TextInfo->GetCursor()->GetPosition();
        }

        if (CommandLineEditingKeys)
        {
            Status = ProcessCommandLine(pCookedReadData, Char, KeyState, pWaitReplyMessage, fWaitRoutine);
            if (Status == CONSOLE_STATUS_READ_COMPLETE || Status == CONSOLE_STATUS_WAIT)
            {
                break;
            }
            if (!NT_SUCCESS(Status))
            {
                if (Status == CONSOLE_STATUS_WAIT_NO_BLOCK)
                {
                    Status = CONSOLE_STATUS_WAIT;
                    if (!fWaitRoutine)
                    {
                        // we have no wait block, so create one.
                        WaitForMoreToRead(pCookedReadData->InputInfo,
                                          pWaitReplyMessage,
                                          (CONSOLE_WAIT_ROUTINE)CookedReadWaitRoutine,
                                          pCookedReadData,
                                          sizeof(*pCookedReadData),
                                          FALSE);
                    }
                }
                else
                {
                    pCookedReadData->BytesRead = 0;
                }
                break;
            }
        }
        else
        {
            if (ProcessCookedReadInput(pCookedReadData, Char, KeyState, &Status))
            {
                g_ciConsoleInformation.Flags |= CONSOLE_IGNORE_NEXT_KEYUP;
                break;
            }
        }
    }

    // if the read was completed (status != wait), free the cooked read
    // data.  also, close the temporary output handle that was opened to
    // echo the characters read.

    if (Status != CONSOLE_STATUS_WAIT)
    {
        DWORD LineCount = 1;

        if (pCookedReadData->Echo)
        {
            BOOLEAN FoundCR;
            ULONG i, StringLength;
            PWCHAR StringPtr;

            // Figure out where real string ends (at carriage return or end of buffer).
            StringPtr = pCookedReadData->BackupLimit;
            StringLength = pCookedReadData->BytesRead;
            FoundCR = FALSE;
            for (i = 0; i < (pCookedReadData->BytesRead / sizeof(WCHAR)); i++)
            {
                if (*StringPtr++ == UNICODE_CARRIAGERETURN)
                {
                    StringLength = i * sizeof(WCHAR);
                    FoundCR = TRUE;
                    break;
                }
            }

            if (FoundCR)
            {
                // add to command line recall list
                AddCommand(pCookedReadData->CommandHistory,
                           pCookedReadData->BackupLimit,
                           (USHORT) StringLength,
                           IsFlagSet(g_ciConsoleInformation.Flags, CONSOLE_HISTORY_NODUP));

                // check for alias
                i = pCookedReadData->BufferSize;
                if (NT_SUCCESS(MatchAndCopyAlias(pCookedReadData->BackupLimit,
                                                 (USHORT) StringLength,
                                                 pCookedReadData->BackupLimit,
                                                 (PUSHORT) & i,
                                                 pCookedReadData->ExeName,
                                                 pCookedReadData->ExeNameLength,
                                                 &LineCount)))
                {
                    pCookedReadData->BytesRead = i;
                }
            }
        }
        SetReplyStatus(pWaitReplyMessage, Status);

        // at this point, a->NumBytes contains the number of bytes in
        // the UNICODE string read.  UserBufferSize contains the converted
        // size of the app's buffer.
        if (pCookedReadData->BytesRead > pCookedReadData->UserBufferSize || LineCount > 1)
        {
            if (LineCount > 1)
            {
                PWSTR Tmp;

                HandleData->pClientInput->InputHandleFlags |= HANDLE_MULTI_LINE_INPUT;
                if (!a->Unicode)
                {
                    if (GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                    {
                        fAddDbcsLead = TRUE;
                        *pCookedReadData->UserBuffer++ = GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                        pCookedReadData->UserBufferSize -= sizeof(WCHAR);
                        ZeroMemory(&GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                    }

                    NumBytes = 0;
                    for (Tmp = pCookedReadData->BackupLimit;
                         *Tmp != UNICODE_LINEFEED && pCookedReadData->UserBufferSize / sizeof(WCHAR) > NumBytes;
                         (IsCharFullWidth(*Tmp) ? NumBytes += 2 : NumBytes++), Tmp++);
                }

#pragma prefast(suppress:__WARNING_BUFFER_OVERFLOW, "LineCount > 1 means there's a UNICODE_LINEFEED")
                for (Tmp = pCookedReadData->BackupLimit; *Tmp != UNICODE_LINEFEED; Tmp++)
                {
                    ASSERT(Tmp < (pCookedReadData->BackupLimit + pCookedReadData->BytesRead));
                }

                a->NumBytes = (ULONG) (Tmp - pCookedReadData->BackupLimit + 1) * sizeof(*Tmp);
            }
            else
            {
                if (!a->Unicode)
                {
                    PWSTR Tmp;

                    if (GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                    {
                        fAddDbcsLead = TRUE;
                        *pCookedReadData->UserBuffer++ = GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                        pCookedReadData->UserBufferSize -= sizeof(WCHAR);
                        ZeroMemory(&GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                    }
                    NumBytes = 0;
                    NumToWrite = pCookedReadData->BytesRead;
                    for (Tmp = pCookedReadData->BackupLimit;
                         NumToWrite && pCookedReadData->UserBufferSize / sizeof(WCHAR) > NumBytes;
                         (IsCharFullWidth(*Tmp) ? NumBytes += 2 : NumBytes++), Tmp++, NumToWrite -= sizeof(WCHAR));
                }
                a->NumBytes = pCookedReadData->UserBufferSize;
            }

            HandleData->pClientInput->InputHandleFlags |= HANDLE_INPUT_PENDING;
            HandleData->pClientInput->BufPtr = pCookedReadData->BackupLimit;
            HandleData->pClientInput->BytesAvailable = pCookedReadData->BytesRead - a->NumBytes;
            HandleData->pClientInput->CurrentBufPtr = (PWCHAR)((PBYTE) pCookedReadData->BackupLimit + a->NumBytes);
            __analysis_assume(a->NumBytes <= pCookedReadData->UserBufferSize);
            memmove(pCookedReadData->UserBuffer, pCookedReadData->BackupLimit, a->NumBytes);
        }
        else
        {
            if (!a->Unicode)
            {
                PWSTR Tmp;

                if (GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
#pragma prefast(suppress:__WARNING_POTENTIAL_BUFFER_OVERFLOW_HIGH_PRIORITY, "This access is legit")
                    *pCookedReadData->UserBuffer++ = GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    pCookedReadData->UserBufferSize -= sizeof(WCHAR);
                    ZeroMemory(&GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));

                    if (pCookedReadData->UserBufferSize == 0)
                    {
                        a->NumBytes = 1;
                        PrepareReadConsoleCompletion(pWaitReplyMessage);
                        delete[] pCookedReadData->BackupLimit;
                        return STATUS_SUCCESS;
                    }
                }
                NumBytes = 0;
                NumToWrite = pCookedReadData->BytesRead;
                for (Tmp = pCookedReadData->BackupLimit;
                     NumToWrite && pCookedReadData->UserBufferSize / sizeof(WCHAR) > NumBytes;
                     (IsCharFullWidth(*Tmp) ? NumBytes += 2 : NumBytes++), Tmp++, NumToWrite -= sizeof(WCHAR));
            }

            a->NumBytes = pCookedReadData->BytesRead;

            if (a->NumBytes > pCookedReadData->UserBufferSize)
            {
                Status = STATUS_BUFFER_OVERFLOW;
                ASSERT(false);
                delete[] pCookedReadData->BackupLimit;
                return Status;
            }

            memmove(pCookedReadData->UserBuffer, pCookedReadData->BackupLimit, a->NumBytes);
            delete[] pCookedReadData->BackupLimit;
        }
        a->ControlKeyState = pCookedReadData->ControlKeyState;

        if (!a->Unicode)
        {
            // if ansi, translate string.
            PCHAR TransBuffer;

            TransBuffer = (PCHAR) new BYTE[NumBytes];

            if (TransBuffer == nullptr)
            {
                return STATUS_NO_MEMORY;
            }

            a->NumBytes = TranslateUnicodeToOem(pCookedReadData->UserBuffer, a->NumBytes / sizeof(WCHAR), TransBuffer, NumBytes, &GetInputBufferFromHandle(HandleData)->ReadConInpDbcsLeadByte);

            if (a->NumBytes > pCookedReadData->UserBufferSize)
            {
                Status = STATUS_BUFFER_OVERFLOW;
                ASSERT(false);
                delete[] TransBuffer;
                return Status;
            }

            memmove(pCookedReadData->UserBuffer, TransBuffer, a->NumBytes);
            if (fAddDbcsLead)
            {
                a->NumBytes++;
            }

            delete[] TransBuffer;
        }

        PrepareReadConsoleCompletion(pWaitReplyMessage);
        delete[] pCookedReadData->ExeName;
        if (fWaitRoutine)
        {
            g_ciConsoleInformation.lpCookedReadData = nullptr;
            CloseOutputHandle(pCookedReadData->ScreenInfo, pCookedReadData->TempHandle);
            delete pCookedReadData;
        }
    }

    return Status;
}

// Routine Description:
// - This routine is called to complete a cooked read that blocked in ReadInputBuffer.
// - The context of the read was saved in the CookedReadData structure.
// - This routine is called when events have been written to the input buffer.
// - It is called in the context of the writing thread.
// - It may be called more than once.
// Arguments:
// - WaitQueue - pointer to queue containing wait block
// - WaitReplyMessage - pointer to reply message
// - CookedReadData - pointer to data saved in ReadChars
// - SatisfyParameter - if this routine is called because a ctrl-c or ctrl-break was seen, this argument
//                      contains CONSOLE_CTRL_SEEN. otherwise it contains nullptr.
// - ThreadDying - Indicates if the owning thread (and process) is exiting.
// Return Value:
BOOL CookedReadWaitRoutine(_In_ PLIST_ENTRY /*pWaitQueue*/,
                           _In_ PCONSOLE_API_MSG pWaitReplyMessage,
                           _In_ PCOOKED_READ_DATA pCookedReadData,
                           _In_ void * const pvSatisfyParameter,
                           _In_ const BOOL fThreadDying)
{
    PCONSOLE_HANDLE_DATA HandleData;
    NTSTATUS Status = DereferenceIoHandleNoCheck(pCookedReadData->HandleIndex, &HandleData);
    if (!NT_SUCCESS(Status))
    {
        return TRUE;
    }
    ASSERT((HandleData->pClientInput->InputHandleFlags & HANDLE_INPUT_PENDING) == 0);

    // this routine should be called by a thread owning the same lock on the same console as we're reading from.
#ifdef DBG
    HandleData->pClientInput->LockReadCount();
    ASSERT(HandleData->pClientInput->GetReadCount() > 0);
    HandleData->pClientInput->UnlockReadCount();
#endif

    HandleData->pClientInput->DecrementReadCount();

    // if ctrl-c or ctrl-break was seen, terminate read.
    if ((ULONG_PTR)pvSatisfyParameter & (CONSOLE_CTRL_C_SEEN | CONSOLE_CTRL_BREAK_SEEN))
    {
        SetReplyStatus(pWaitReplyMessage, STATUS_ALERTED);
        delete[] pCookedReadData->BackupLimit;
        delete[] pCookedReadData->ExeName;
        g_ciConsoleInformation.lpCookedReadData = nullptr;
        CloseOutputHandle(pCookedReadData->ScreenInfo, pCookedReadData->TempHandle);
        delete pCookedReadData;
        return TRUE;
    }

    // See if we were called because the thread that owns this wait block is exiting.
    if (fThreadDying)
    {
        SetReplyStatus(pWaitReplyMessage, STATUS_THREAD_IS_TERMINATING);

        // Clean up popup data structures.
        CleanUpPopups(pCookedReadData);
        delete[] pCookedReadData->BackupLimit;
        delete[] pCookedReadData->ExeName;
        g_ciConsoleInformation.lpCookedReadData = nullptr;
        CloseOutputHandle(pCookedReadData->ScreenInfo, pCookedReadData->TempHandle);
        delete pCookedReadData;
        return TRUE;
    }

    // We must see if we were woken up because the handle is being closed. If
    // so, we decrement the read count. If it goes to zero, we wake up the
    // close thread. Otherwise, we wake up any other thread waiting for data.

    if ((HandleData->pClientInput->InputHandleFlags & HANDLE_CLOSING) != 0)
    {
        SetReplyStatus(pWaitReplyMessage, STATUS_ALERTED);

        // Clean up popup data structures.
        CleanUpPopups(pCookedReadData);
        delete[] pCookedReadData->BackupLimit;
        delete[] pCookedReadData->ExeName;
        g_ciConsoleInformation.lpCookedReadData = nullptr;
        CloseOutputHandle(pCookedReadData->ScreenInfo, pCookedReadData->TempHandle);
        delete pCookedReadData;
        return TRUE;
    }

    // If we get to here, this routine was called either by the input thread
    // or a write routine. Both of these callers grab the current console
    // lock.

    // this routine should be called by a thread owning the same
    // lock on the same console as we're reading from.

    ASSERT(g_ciConsoleInformation.IsConsoleLocked());

    if (pCookedReadData->CommandHistory)
    {
        PCLE_POPUP Popup;
        if (!CLE_NO_POPUPS(pCookedReadData->CommandHistory))
        {
            Popup = CONTAINING_RECORD(pCookedReadData->CommandHistory->PopupList.Flink, CLE_POPUP, ListLink);
            Status = (Popup->PopupInputRoutine) (pCookedReadData, pWaitReplyMessage, TRUE);
            if (Status == CONSOLE_STATUS_READ_COMPLETE || (Status != CONSOLE_STATUS_WAIT && Status != CONSOLE_STATUS_WAIT_NO_BLOCK))
            {
                delete[] pCookedReadData->BackupLimit;
                delete[] pCookedReadData->ExeName;
                g_ciConsoleInformation.lpCookedReadData = nullptr;
                CloseOutputHandle(pCookedReadData->ScreenInfo, pCookedReadData->TempHandle);
                delete pCookedReadData;

                if (NT_SUCCESS(pWaitReplyMessage->Complete.IoStatus.Status))
                {
                    PrepareReadConsoleCompletion(pWaitReplyMessage);
                }

                return TRUE;
            }
            return FALSE;
        }
    }

    Status = CookedRead(pCookedReadData, pWaitReplyMessage, TRUE);
    if (Status != CONSOLE_STATUS_WAIT)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

// Routine Description:
// - This routine reads in characters for stream input and does the required processing based on the input mode (line, char, echo).
// - This routine returns UNICODE characters.
// Arguments:
// - InputInfo - Pointer to input buffer information.
// - Console - Pointer to console buffer information.
// - ScreenInfo - Pointer to screen buffer information.
// - lpBuffer - Pointer to buffer to read into.
// - NumBytes - On input, size of buffer.  On output, number of bytes read.
// - HandleData - Pointer to handle data structure.
// Return Value:
NTSTATUS ReadChars(_In_ PINPUT_INFORMATION const pInputInfo,
                   _In_ PCONSOLE_PROCESS_HANDLE const pProcessData,
                   _In_ PSCREEN_INFORMATION const pScreenInfo,
                   _Inout_updates_bytes_(*pdwNumBytes) PWCHAR pwchBuffer,
                   _Inout_ PDWORD pdwNumBytes,
                   _In_ DWORD const dwInitialNumBytes,
                   _In_ DWORD const dwCtrlWakeupMask,
                   _In_ PCONSOLE_HANDLE_DATA const pHandleData,
                   _In_ PCOMMAND_HISTORY const pCommandHistory,
                   _In_opt_ PCONSOLE_API_MSG const pMessage,
                   _In_ HANDLE const hIndex,
                   _In_ USHORT const usExeNameLength,
                   _In_ BOOLEAN const fUnicode)
{
    NTSTATUS Status;
    ULONG NumToWrite;
    BOOL fAddDbcsLead = FALSE;
    ULONG NumToBytes = 0;
    USHORT ExeNameByteLength;

    if (*pdwNumBytes < sizeof(WCHAR))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        return Status;
    }

    DWORD BufferSize = *pdwNumBytes;
    *pdwNumBytes = 0;

    if (pHandleData->pClientInput->InputHandleFlags & HANDLE_INPUT_PENDING)
    {
        // if we have leftover input, copy as much fits into the user's
        // buffer and return.  we may have multi line input, if a macro
        // has been defined that contains the $T character.

        if (pHandleData->pClientInput->InputHandleFlags & HANDLE_MULTI_LINE_INPUT)
        {
            PWSTR Tmp;

            if (!fUnicode)
            {
                if (GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
                    *pwchBuffer++ = GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    BufferSize -= sizeof(WCHAR);
                    pHandleData->pClientInput->BytesAvailable -= sizeof(WCHAR);
                    ZeroMemory(&GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                }

                if (pHandleData->pClientInput->BytesAvailable == 0 || BufferSize == 0)
                {
                    pHandleData->pClientInput->InputHandleFlags &= ~(HANDLE_INPUT_PENDING | HANDLE_MULTI_LINE_INPUT);
                    delete[] pHandleData->pClientInput->BufPtr;
                    *pdwNumBytes = 1;
                    return STATUS_SUCCESS;
                }
                else
                {
                    for (NumToWrite = 0, Tmp = pHandleData->pClientInput->CurrentBufPtr, NumToBytes = 0;
                         NumToBytes < pHandleData->pClientInput->BytesAvailable && NumToBytes < BufferSize / sizeof(WCHAR) && *Tmp != UNICODE_LINEFEED;
                         (IsCharFullWidth(*Tmp) ? NumToBytes += 2 : NumToBytes++), Tmp++, NumToWrite += sizeof(WCHAR));
                }
            }

            for (NumToWrite = 0, Tmp = pHandleData->pClientInput->CurrentBufPtr;
                 NumToWrite < pHandleData->pClientInput->BytesAvailable && *Tmp != UNICODE_LINEFEED; Tmp++, NumToWrite += sizeof(WCHAR));
            NumToWrite += sizeof(WCHAR);
            if (NumToWrite > BufferSize)
            {
                NumToWrite = BufferSize;
            }
        }
        else
        {
            if (!fUnicode)
            {
                PWSTR Tmp;

                if (GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
                    *pwchBuffer++ = GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    BufferSize -= sizeof(WCHAR);
                    pHandleData->pClientInput->BytesAvailable -= sizeof(WCHAR);
                    ZeroMemory(&GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                }
                if (pHandleData->pClientInput->BytesAvailable == 0)
                {
                    pHandleData->pClientInput->InputHandleFlags &= ~(HANDLE_INPUT_PENDING | HANDLE_MULTI_LINE_INPUT);
                    delete[] pHandleData->pClientInput->BufPtr;
                    *pdwNumBytes = 1;
                    return STATUS_SUCCESS;
                }
                else
                {
                    for (NumToWrite = 0, Tmp = pHandleData->pClientInput->CurrentBufPtr, NumToBytes = 0;
                         NumToBytes < pHandleData->pClientInput->BytesAvailable && NumToBytes < BufferSize / sizeof(WCHAR);
                         (IsCharFullWidth(*Tmp) ? NumToBytes += 2 : NumToBytes++), Tmp++, NumToWrite += sizeof(WCHAR));
                }
            }

            NumToWrite = (BufferSize < pHandleData->pClientInput->BytesAvailable) ? BufferSize : pHandleData->pClientInput->BytesAvailable;
        }

        memmove(pwchBuffer, pHandleData->pClientInput->CurrentBufPtr, NumToWrite);
        pHandleData->pClientInput->BytesAvailable -= NumToWrite;
        if (pHandleData->pClientInput->BytesAvailable == 0)
        {
            pHandleData->pClientInput->InputHandleFlags &= ~(HANDLE_INPUT_PENDING | HANDLE_MULTI_LINE_INPUT);
            delete[] pHandleData->pClientInput->BufPtr;
        }
        else
        {
            pHandleData->pClientInput->CurrentBufPtr = (PWCHAR)((PBYTE) pHandleData->pClientInput->CurrentBufPtr + NumToWrite);
        }

        if (!fUnicode)
        {
            // if ansi, translate string.  we allocated the capture buffer large enough to handle the translated string.
            PCHAR TransBuffer;

            TransBuffer = (PCHAR) new BYTE[NumToBytes];

            if (TransBuffer == nullptr)
            {
                return STATUS_NO_MEMORY;
            }

            NumToWrite = TranslateUnicodeToOem(pwchBuffer, NumToWrite / sizeof(WCHAR), TransBuffer, NumToBytes, &GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte);

#pragma prefast(suppress:__WARNING_POTENTIAL_BUFFER_OVERFLOW_HIGH_PRIORITY, "This access is fine but prefast can't follow it, evidently")
            memmove(pwchBuffer, TransBuffer, NumToWrite);

            if (fAddDbcsLead)
            {
                NumToWrite++;
            }

            delete[] TransBuffer;
        }

        *pdwNumBytes = NumToWrite;
        return STATUS_SUCCESS;
    }

    if (pInputInfo->InputMode & ENABLE_LINE_INPUT)
    {
        // read in characters until the buffer is full or return is read.
        // since we may wait inside this loop, store all important variables
        // in the read data structure.  if we do wait, a read data structure
        // will be allocated from the heap and its pointer will be stored
        // in the wait block.  the CookedReadData will be copied into the
        // structure.  the data is freed when the read is completed.

        COOKED_READ_DATA CookedReadData;
        ULONG i;
        PWCHAR TempBuffer;
        ULONG TempBufferSize;
        BOOLEAN Echo;

        // We need to create a temporary handle to the current screen buffer.
        Status = AllocateIoHandle(CONSOLE_OUTPUT_HANDLE, &CookedReadData.TempHandle, &pScreenInfo->Header, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Echo = !!(pInputInfo->InputMode & ENABLE_ECHO_INPUT);


        // to emulate OS/2 KbdStringIn, we read into our own big buffer
        // (256 bytes) until the user types enter.  then return as many
        // chars as will fit in the user's buffer.

        TempBufferSize = (BufferSize < LINE_INPUT_BUFFER_SIZE) ? LINE_INPUT_BUFFER_SIZE : BufferSize;
        TempBuffer = (PWCHAR) new BYTE[TempBufferSize];
        if (TempBuffer == nullptr)
        {
            if (Echo)
            {
                CloseOutputHandle(pScreenInfo, CookedReadData.TempHandle);
            }

            return STATUS_NO_MEMORY;
        }

        // Initialize the user's buffer to spaces. This is done so that
        // moving in the buffer via cursor doesn't do strange things.
        for (i = 0; i < TempBufferSize / sizeof(WCHAR); i++)
        {
            TempBuffer[i] = (WCHAR)' ';
        }

        /*
         * Since the console is locked, ScreenInfo is safe. We need to up the
         * ref count to prevent the ScreenInfo from going away while we're
         * waiting for the read to complete.
         */
        CookedReadData.InputInfo = pInputInfo;
        CookedReadData.ScreenInfo = pScreenInfo;
        CookedReadData.BufferSize = TempBufferSize;
        CookedReadData.BytesRead = 0;
        CookedReadData.CurrentPosition = 0;
        CookedReadData.BufPtr = TempBuffer;
        CookedReadData.BackupLimit = TempBuffer;
        CookedReadData.UserBufferSize = BufferSize;
        CookedReadData.UserBuffer = pwchBuffer;
        CookedReadData.OriginalCursorPosition.X = -1;
        CookedReadData.OriginalCursorPosition.Y = -1;
        CookedReadData.NumberOfVisibleChars = 0;
        CookedReadData.CtrlWakeupMask = dwCtrlWakeupMask;
        CookedReadData.CommandHistory = pCommandHistory;
        CookedReadData.Echo = Echo;
        CookedReadData.InsertMode = !!g_ciConsoleInformation.GetInsertMode();
        CookedReadData.Processed = (pInputInfo->InputMode & ENABLE_PROCESSED_INPUT) != 0;
        CookedReadData.Line = (pInputInfo->InputMode & ENABLE_LINE_INPUT) != 0;
        CookedReadData.ProcessData = pProcessData;
        CookedReadData.HandleIndex = hIndex;

        ExeNameByteLength = (USHORT) (usExeNameLength * sizeof(WCHAR));
        CookedReadData.ExeName = (PWCHAR) new BYTE[ExeNameByteLength];

        if (dwInitialNumBytes != 0)
        {
            ReadMessageInput(pMessage, ExeNameByteLength, CookedReadData.BufPtr, dwInitialNumBytes);

            CookedReadData.BytesRead += dwInitialNumBytes;
            CookedReadData.NumberOfVisibleChars = (dwInitialNumBytes / sizeof(WCHAR));
            CookedReadData.BufPtr += (dwInitialNumBytes / sizeof(WCHAR));
            CookedReadData.CurrentPosition = (dwInitialNumBytes / sizeof(WCHAR));
            CookedReadData.OriginalCursorPosition = pScreenInfo->TextInfo->GetCursor()->GetPosition();
            CookedReadData.OriginalCursorPosition.X -= (SHORT)CookedReadData.CurrentPosition;


            while (CookedReadData.OriginalCursorPosition.X < 0)
            {
                CookedReadData.OriginalCursorPosition.X += pScreenInfo->ScreenBufferSize.X;
                CookedReadData.OriginalCursorPosition.Y -= 1;
            }
        }

        if (CookedReadData.ExeName)
        {
            ReadMessageInput(pMessage, 0, CookedReadData.ExeName, ExeNameByteLength);
            CookedReadData.ExeNameLength = ExeNameByteLength;
        }

        g_ciConsoleInformation.lpCookedReadData = &CookedReadData;

        Status = CookedRead(&CookedReadData, pMessage, FALSE);
        if (Status != CONSOLE_STATUS_WAIT)
        {
            g_ciConsoleInformation.lpCookedReadData = nullptr;
        }

        return Status;
    }
    else
    {
        // Character (raw) mode.

        // Read at least one character in. After one character has been read,
        // get any more available characters and return. The first call to
        // GetChar may block. If we do wait, a read data structure will be
        // allocated from the heap and its pointer will be stored in the wait
        // block. The RawReadData will be copied into the structure. The data
        // is freed when the read is completed.

        RAW_READ_DATA RawReadData;

        RawReadData.InputInfo = pInputInfo;
        RawReadData.BufferSize = BufferSize;
        RawReadData.BufPtr = pwchBuffer;
        RawReadData.ProcessData = pProcessData;
        RawReadData.HandleIndex = hIndex;
        if (*pdwNumBytes < BufferSize)
        {
            PWCHAR pwchBufferTmp = pwchBuffer;

            NumToWrite = 0;
            if (!fUnicode)
            {
                if (GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar)
                {
                    fAddDbcsLead = TRUE;
                    *pwchBuffer++ = GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte.Event.KeyEvent.uChar.AsciiChar;
                    BufferSize -= sizeof(WCHAR);
                    ZeroMemory(&GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
                    Status = STATUS_SUCCESS;
                    if (BufferSize == 0)
                    {
                        *pdwNumBytes = 1;
                        return STATUS_SUCCESS;
                    }
                }
                else
                {
                    Status = GetChar(pInputInfo,
                                     pwchBuffer,
                                     TRUE,
                                     pHandleData,
                                     pMessage,
                                     RawReadWaitRoutine,
                                     &RawReadData,
                                     sizeof(RawReadData),
                                     FALSE,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr);
                }
            }
            else
            {
                Status = GetChar(pInputInfo,
                                 pwchBuffer,
                                 TRUE,
                                 pHandleData,
                                 pMessage,
                                 RawReadWaitRoutine,
                                 &RawReadData,
                                 sizeof(RawReadData),
                                 FALSE,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr);
            }

            if (!NT_SUCCESS(Status))
            {
                *pdwNumBytes = 0;
                return Status;
            }

            if (!fAddDbcsLead)
            {
                IsCharFullWidth(*pwchBuffer) ? *pdwNumBytes += 2 : ++*pdwNumBytes;
                NumToWrite += sizeof(WCHAR);
                pwchBuffer++;
            }

            while (NumToWrite < BufferSize)
            {
                Status = GetChar(pInputInfo,
                                    pwchBuffer,
                                    FALSE,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    0,
                                    FALSE,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
                if (!NT_SUCCESS(Status))
                {
                    return STATUS_SUCCESS;
                }
                IsCharFullWidth(*pwchBuffer) ? *pdwNumBytes += 2 : ++*pdwNumBytes;
                pwchBuffer++;
                NumToWrite += sizeof(WCHAR);
            }

            // if ansi, translate string.  we allocated the capture buffer large enough to handle the translated string.
            if (!fUnicode)
            {
                PCHAR TransBuffer;

                TransBuffer = (PCHAR) new BYTE[*pdwNumBytes];
                if (TransBuffer == nullptr)
                {
                    return STATUS_NO_MEMORY;
                }

                pwchBuffer = pwchBufferTmp;
               
                *pdwNumBytes = TranslateUnicodeToOem(pwchBuffer,
                                                        NumToWrite / sizeof(WCHAR),
                                                        TransBuffer,
                                                        *pdwNumBytes,
                                                        &GetInputBufferFromHandle(pHandleData)->ReadConInpDbcsLeadByte);

#pragma prefast(suppress:26053 26015, "PREfast claims read overflow. *pdwNumBytes is the exact size of TransBuffer as allocated above.")
                memmove(pwchBuffer, TransBuffer, *pdwNumBytes);

                if (fAddDbcsLead)
                {
                    ++*pdwNumBytes;
                }

                delete[] TransBuffer;
            }
        }
    }

    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine reads characters from the input stream.
NTSTATUS SrvReadConsole(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL ReplyPending)
{
    PCONSOLE_READCONSOLE_MSG const a = &m->u.consoleMsgL1.ReadConsole;

    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::ReadConsole, a->Unicode);

    PWCHAR Buffer;
    // If the request is not in Unicode mode, we must allocate an output buffer that is twice as big as the actual caller buffer.
    NTSTATUS Status = GetAugmentedOutputBuffer(m, (a->Unicode != FALSE) ? 1 : 2, (PVOID *) & Buffer, &a->NumBytes);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    CONSOLE_INFORMATION *Console;
    Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    PCONSOLE_PROCESS_HANDLE const ProcessData = GetMessageProcess(m);
    
    PCONSOLE_HANDLE_DATA HandleData;
    Status = DereferenceIoHandle(GetMessageObject(m), CONSOLE_INPUT_HANDLE, GENERIC_READ, &HandleData);
    if (!NT_SUCCESS(Status))
    {
        a->NumBytes = 0;
    }
    else
    {
        if (a->InitialNumBytes > a->NumBytes)
        {
            UnlockConsole();
            return STATUS_INVALID_PARAMETER;
        }

        if (g_ciConsoleInformation.CurrentScreenBuffer != nullptr)
        {
            Status = ReadChars(GetInputBufferFromHandle(HandleData),
                               ProcessData,
                               g_ciConsoleInformation.CurrentScreenBuffer,
                               Buffer,
                               &a->NumBytes,
                               a->InitialNumBytes,
                               a->CtrlWakeupMask,
                               HandleData,
                               FindCommandHistory((HANDLE) ProcessData),
                               m,
                               GetMessageObject(m),
                               a->ExeNameLength,
                               a->Unicode);
            if (Status == CONSOLE_STATUS_WAIT)
            {
                *ReplyPending = TRUE;
            }
        }
        else
        {
            Status = STATUS_UNSUCCESSFUL;
        }
    }

    UnlockConsole();

    if (NT_SUCCESS(Status))
    {
        PrepareReadConsoleCompletion(m);
    }

    return Status;
}

VOID UnblockWriteConsole(_In_ const DWORD dwReason)
{
    g_ciConsoleInformation.Flags &= ~dwReason;

    if ((g_ciConsoleInformation.Flags & (CONSOLE_SUSPENDED | CONSOLE_SELECTING | CONSOLE_SCROLLBAR_TRACKING)) == 0)
    {
        // There is no longer any reason to suspend output, so unblock it.
        ConsoleNotifyWait(&g_ciConsoleInformation.OutputQueue, TRUE, nullptr);
    }
}

// Routine Description:
// - This routine writes characters to the output stream.
NTSTATUS SrvWriteConsole(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL ReplyPending)
{
    PCONSOLE_WRITECONSOLE_MSG const a = &m->u.consoleMsgL1.WriteConsole;

    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::WriteConsole, a->Unicode);

    PVOID Buffer;
    NTSTATUS Status = GetInputBuffer(m, &Buffer, &a->NumBytes);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    CONSOLE_INFORMATION *Console;
    Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Make sure we have a valid screen buffer.
    PCONSOLE_HANDLE_DATA HandleData;
    Status = DereferenceIoHandle(GetMessageObject(m), CONSOLE_OUTPUT_HANDLE, GENERIC_WRITE, &HandleData);
    if (NT_SUCCESS(Status))
    {
        Status = DoSrvWriteConsole(m, ReplyPending, Buffer, HandleData);
    }

    UnlockConsole();

    return Status;
}

BOOL WriteConsoleWaitRoutine(_In_ PLIST_ENTRY /*pWaitQueue*/,
                             _In_ PCONSOLE_API_MSG pWaitReplyMessage,
                             _In_ PVOID pvWaitParameter,
                             _In_ PVOID /*pvSatisfyParameter*/,
                             _In_ BOOL fThreadDying)
{
    PCONSOLE_WRITECONSOLE_MSG const a = &pWaitReplyMessage->u.consoleMsgL1.WriteConsole;

    if (fThreadDying)
    {
        SetReplyStatus(pWaitReplyMessage, STATUS_THREAD_IS_TERMINATING);
        return TRUE;
    }

    // if we get to here, this routine was called by the input
    // thread, which grabs the current console lock.

    // This routine should be called by a thread owning the same lock on the
    // same console as we're reading from.

    ASSERT(g_ciConsoleInformation.IsConsoleLocked());

    NTSTATUS Status = DoWriteConsole(pWaitReplyMessage, (PSCREEN_INFORMATION)pvWaitParameter);
    if (Status == CONSOLE_STATUS_WAIT)
    {
        return FALSE;
    }

    if (!a->Unicode)
    {
        if (a->NumBytes == g_ciConsoleInformation.WriteConOutNumBytesUnicode)
        {
            a->NumBytes = g_ciConsoleInformation.WriteConOutNumBytesTemp;
        }
        else
        {
            a->NumBytes /= sizeof(WCHAR);
        }

        delete[] pWaitReplyMessage->State.TransBuffer;
    }

    SetReplyStatus(pWaitReplyMessage, Status);
    SetReplyInformation(pWaitReplyMessage, a->NumBytes);
    return TRUE;
}

// Routine Description:
// - This routine closes an input handle.  It decrements the input buffer's reference count.
//   If it goes to zero, the buffer is reinitialized. Otherwise, the handle is removed from sharing.
// Arguments:
// - ProcessData - Pointer to per process data.
// - HandleData - Pointer to handle data structure.
// - Handle - Handle to close.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS CloseInputHandle(_In_ PCONSOLE_HANDLE_DATA pHandleData, _In_ const HANDLE hClose)
{
    if (pHandleData->pClientInput->InputHandleFlags & HANDLE_INPUT_PENDING)
    {
        pHandleData->pClientInput->InputHandleFlags &= ~HANDLE_INPUT_PENDING;
        delete[] pHandleData->pClientInput->BufPtr;
    }

    PINPUT_INFORMATION const InputBuffer = GetInputBufferFromHandle(pHandleData);

    // see if there are any reads waiting for data via this handle.  if
    // there are, wake them up.  there aren't any other outstanding i/o
    // operations via this handle because the console lock is held.

    pHandleData->pClientInput->LockReadCount();
    if (pHandleData->pClientInput->GetReadCount() != 0)
    {
        pHandleData->pClientInput->UnlockReadCount();
        pHandleData->pClientInput->InputHandleFlags |= HANDLE_CLOSING;

        ConsoleNotifyWait(&InputBuffer->ReadWaitQueue, TRUE, nullptr);

        pHandleData->pClientInput->LockReadCount();
    }

    ASSERT(pHandleData->pClientInput->GetReadCount() == 0);
    pHandleData->pClientInput->UnlockReadCount();

    delete pHandleData->pClientInput;

    if (FreeConsoleHandle(hClose))
    {
        ReinitializeInputBuffer(InputBuffer);
    }

    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine closes an output handle.  It decrements the screen buffer's reference count.
// - If it goes to zero, the buffer is freed.  Otherwise, the handle is removed from sharing.
// Arguments:
// - ProcessData - Pointer to per process data.
// - Console - Pointer to console information structure.
// - HandleData - Pointer to handle data structure.
// - Handle - Handle to close.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS CloseOutputHandle(_In_ PSCREEN_INFORMATION pScreenInfo, _In_ const HANDLE hClose)
{
    if (FreeConsoleHandle(hClose))
    {
        RemoveScreenBuffer(pScreenInfo);
    }

    return STATUS_SUCCESS;
}

NTSTATUS SrvCloseHandle(_In_ PCONSOLE_API_MSG m)
{
    CONSOLE_INFORMATION *Console;
    NTSTATUS Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ConsoleCloseHandle(GetMessageObject(m));

    UnlockConsole();
    return Status;
}