#include "StdAfx.h"
#include "CommunicationController.h"
#include "../Configuration/configuration.h"
#include "FTPCom.h"
#include "FTPHandler.h"
#include "SerialControllerWithTx.h"
#include "NodeControlInfo.h"


using namespace Communication;
#define SMALL_NODE_SUM 5

UINT ConnectBySerialWithTX(LPVOID pParam);
UINT ConnectByFTP(LPVOID pParam);
void Pause(double startTime, double stopTime, int serialID);

void UploadFile_SerialTx(int i, Communication::CCommunicationController *mainController, Communication::CSerialControllerWithTx *cable);
void UploadFile_FTP(int mainIndex, Communication::CFTPHandler* ftpHandler);


IMPLEMENT_DYNCREATE(CCommunicationController, CWinThread)

BEGIN_MESSAGE_MAP(CCommunicationController, CWinThread)
    ON_THREAD_MESSAGE(WM_QUIT, OnQuit)
    ON_THREAD_MESSAGE(WM_UPLOAD_CFGONCE, OnUploadCfgOnce)
END_MESSAGE_MAP()

extern CConfigurationSetting g_settings; // <-- the configuration
bool g_runFlag;

// This is an array of files to upload to the instruments
//	the element 'g_fileToUpload[i]' contains the name and path
//	of the file to upload to instrument with mainIndex=i
CArray <CString, CString &> g_fileToUpload;

CCommunicationController::CCommunicationController()
{
    //the sum of the serial connections
    m_totalSerialConnection = 0;

    //the sum of the ftp connections
    m_totalFTPConnection = 0;

    //the sum of the http connections
    m_totalHttpConnection = 0;

    //set the size of serial information list
    m_serialList.SetSize(SMALL_NODE_SUM);

    //set member array of serial controller object
    m_SerialControllerTx.SetSize(SMALL_NODE_SUM);

    //set member array of ftp handler object
    m_FTPHandler.SetSize(SMALL_NODE_SUM);
    m_nodeControl.reset(new CNodeControlInfo());
    g_runFlag = true;
}

CCommunicationController::~CCommunicationController()
{
}

//-----finsh downloading file control ----
/** Quits the thread */
void CCommunicationController::OnQuit(WPARAM wp, LPARAM lp) {

}
BOOL CCommunicationController::InitInstance()
{
    CWinThread::InitInstance();

    //1. start every threads according to the configuration file
    StartCommunicationThreads();

    return TRUE;
}

void CCommunicationController::OnUploadCfgOnce(WPARAM wp, LPARAM lp)
{
    //set spectrometer id from param wp and mode from param lp
    CString* spectrometerID = (CString*)wp;
    CString* filePath = (CString*)lp;
    CString message;

    // Check the data...
    if (filePath == NULL || !IsExistingFile(*filePath)) {
        ShowMessage("Recieved upload command for non-existing file");
        delete spectrometerID;
        delete filePath;
        return;
    }

    int mainIndex = m_nodeControl->GetMainIndex(*spectrometerID);
    if (mainIndex < 0 || mainIndex >= g_settings.scannerNum) {
        ShowMessage("Received upload command for non-existing instrument");
        delete spectrometerID;
        delete filePath;
        return;
    }

    // Store the name to upload
    if (FTP_CONNECTION == g_settings.scanner[mainIndex].comm.connectionType) {
        g_fileToUpload.SetAtGrow(mainIndex, *filePath);
    }
    else
    {
        m_nodeControl->SetNodeStatus(mainIndex, DeviceMode::Special, *filePath);
    }

    message.Format("File %s added to upload-queue for node %d", (LPCSTR)*filePath, mainIndex);
    ShowMessage(message);

    // clear up...
    delete spectrometerID;
    delete filePath;
}

void CCommunicationController::StartCommunicationThreads()
{
    // Clear all lists and variables...
    m_totalSerialConnection = 0;
    m_totalFTPConnection = 0;
    m_totalHttpConnection = 0;
    m_ftpList.RemoveAll();
    m_serialList.RemoveAll();
    m_httpList.RemoveAll();

    // Allocate enough size for the buffer of files to upload
    g_fileToUpload.SetSize(g_settings.scannerNum);

    for (unsigned long i = 0; i < g_settings.scannerNum; ++i)
    {
        CConfigurationSetting::CommunicationSetting &comm = g_settings.scanner[i].comm;
        ELECTRONICS_BOX	box = g_settings.scanner[i].electronicsBox;

        unsigned int connectionType = comm.connectionType;

        m_nodeControl->FillinNodeInfo(i, DeviceMode::Sleep, g_settings.scanner[i].spec[0].serialNumber, "");

        switch (connectionType)
        {
            //when it is serial connection
        case SERIAL_CONNECTION:
            m_serialList.SetAtGrow(m_totalSerialConnection, new CSerialInfo(i, false, comm.medium));
            m_totalSerialConnection++;
            break;
            //when it is ftp connection
        case FTP_CONNECTION:
            m_totalFTPConnection++;
            m_ftpList.AddTail(i);
            break;
            //when it is http connection
        case HTTP_CONNECTION:
            m_totalHttpConnection = 1;
            m_httpList.AddTail(i);
            break;
        default:
            break;
        }
    }

    if (m_totalSerialConnection > 0)
    {
        AfxBeginThread(ConnectBySerialWithTX, this, THREAD_PRIORITY_NORMAL, 0, 0, NULL);
    }

    if (m_totalFTPConnection > 0)
    {
        StartFTP();
    }
}

void CCommunicationController::StartFTP()
{
    POSITION pos = m_ftpList.GetHeadPosition();

    // 1. Make sure that there are no duplicates in the list
    if (m_ftpList.GetSize() > 1) {
        while (pos != NULL) {
            POSITION pos2 = pos;
            m_ftpList.GetNext(pos2);
            while (pos2 != NULL) {
                int index = m_ftpList.GetNext(pos2);
                if (index == m_ftpList.GetAt(pos)) {
                    // there are duplicates in the list. Start over!
                    m_ftpList.RemoveAt(pos2);
                    StartFTP();
                    return;
                }
            }
            m_ftpList.GetNext(pos);
        }
    }

    // 2. Start the FTP-threads
    pos = m_ftpList.GetHeadPosition();
    while (pos != NULL) {
        int *index = new int;
        *index = m_ftpList.GetNext(pos);
        AfxBeginThread(ConnectByFTP, index, THREAD_PRIORITY_NORMAL, 0, 0, NULL);
    }
}

/** connect to remote PC by FTP method.
*@param pParam - the object of the CommunicationController
*/
UINT ConnectByFTP(LPVOID pParam)
{
    int i = 0;
    long nRoundsAfterWakeUp = 0;
    int mainIndex = *(int*)pParam;
    bool sleepFlag = false;
    clock_t startTime, stopTime;
    long sleepPeriod;
    CString remoteFile, message, ip, spectrometerSerialID;

    // Setup the ftp-handler for this connection
    CFTPHandler* ftpHandler;
    ftpHandler = new CFTPHandler(g_settings.scanner[mainIndex].electronicsBox);
    ip.Format("%d.%d.%d.%d", g_settings.scanner[mainIndex].comm.ftpIP[0],
        g_settings.scanner[mainIndex].comm.ftpIP[1],
        g_settings.scanner[mainIndex].comm.ftpIP[2],
        g_settings.scanner[mainIndex].comm.ftpIP[3]);
    ftpHandler->SetFTPInfo(mainIndex, ip, g_settings.scanner[mainIndex].comm.ftpUserName,
        g_settings.scanner[mainIndex].comm.ftpPassword,
        g_settings.scanner[mainIndex].comm.ftpAdminUserName,
        g_settings.scanner[mainIndex].comm.ftpAdminPassword,
        g_settings.scanner[mainIndex].comm.timeout / 1000);

    spectrometerSerialID.Format("%s", (LPCSTR)g_settings.scanner[mainIndex].spec[0].serialNumber);

    while (g_runFlag)
    {
        // --------------- CHECK IF WE SHOULD GO TO SLEEP OR WAKE UP ---------------
        sleepPeriod = GetSleepTime(g_settings.scanner[mainIndex].comm.sleepTime, g_settings.scanner[mainIndex].comm.wakeupTime);

        //judge sleep time
        if (sleepPeriod > 0)
        {
            sleepFlag = true;
            ftpHandler->GotoSleep();
            Sleep(sleepPeriod);
            nRoundsAfterWakeUp = 0;
            continue;
        }
        else
        {
            if (sleepFlag || nRoundsAfterWakeUp == 0)
            {
                ftpHandler->WakeUp();
                sleepFlag = false;
            }
        }

        // ---------- Check if we should upload something to the instrument ------
        if (mainIndex < g_fileToUpload.GetCount())
        {
            UploadFile_FTP(mainIndex, ftpHandler);
        }

        // ----------- DOWNLOAD DATA ------------------
        startTime = clock();
        ftpHandler->PollScanner();
        stopTime = clock();

        // if there's no file to upload then we can take a pause...
        if (mainIndex >= 0 && (mainIndex < g_fileToUpload.GetCount()) && g_fileToUpload.GetAt(mainIndex).GetLength() <= 4) {
            Pause(startTime, stopTime, mainIndex);
        }
        nRoundsAfterWakeUp++;
    }

    // Quit!
    ftpHandler->Disconnect();
    return 0;
}

void UploadFile_SerialTx(int i, Communication::CCommunicationController *mainController, CSerialControllerWithTx *cable) {
    CString strFileName;		//file name in remote PC in CString type
    CString uploadFilePath; // full file path to be uploaded in special_mode
    CString uploadFileFolder; // the folder that the file to be uploaded locates
    char fileName[56];			//file name in remote PC
    Common common;

    //run the special mode,upload special file
    mainController->m_nodeControl->GetNodeCfgFilePath(i, strFileName);
    if (IsExistingFile(strFileName)) {
        uploadFilePath.Format(strFileName);
        uploadFileFolder.Format(strFileName);
        common.GetDirectory(uploadFileFolder);
        common.GetFileName(strFileName);
        sprintf(fileName, "%s", (LPCSTR)strFileName);

        cable->UploadFile(uploadFileFolder, fileName, 'A');
        mainController->m_nodeControl->SetNodeStatus(i, DeviceMode::Run, uploadFilePath);
        cable->CloseSerialPort();
    }
}

void UploadFile_FTP(int mainIndex, CFTPHandler* ftpHandler)
{
    CString ip, message, remoteFile;

    CString &fileName = g_fileToUpload.GetAt(mainIndex);
    if (fileName.GetLength() > 4) {
        // Get the name of the remote file...
        remoteFile.Format(fileName);
        Common::GetFileName(remoteFile);

        ip.Format("%d.%d.%d.%d", g_settings.scanner[mainIndex].comm.ftpIP[0],
            g_settings.scanner[mainIndex].comm.ftpIP[1],
            g_settings.scanner[mainIndex].comm.ftpIP[2],
            g_settings.scanner[mainIndex].comm.ftpIP[3]);

        // Connect to the server
        if (ftpHandler->Connect(ip, "administrator", "1225", g_settings.scanner[mainIndex].comm.timeout)) {

            // Upload the file
            if (ftpHandler->UploadFile(fileName, remoteFile)) {
                message.Format("Failed to upload %s to node %d", (LPCSTR)fileName, mainIndex);
            }
            else {
                message.Format("Successfully uploaded %s to node %d", (LPCSTR)fileName, mainIndex);
            }
            ShowMessage(message);

            // Disconnect again...
            ftpHandler->Disconnect();
        }
        else {
            message.Format("Cannot connect to administrator account on node %d", mainIndex);
            ShowMessage(message);
        }

        // Remove the string, so we don't upload the file again...
        fileName.Format("");
        g_fileToUpload.SetAt(mainIndex, fileName);
    }
}


/** connect to remote PC by serial method. This function uses Manne's tx serial
* communication protocol
*@param pParam - the object of the CommunicationController
*/
UINT ConnectBySerialWithTX(LPVOID pParam)
{
    CString msg, timetxt;
    long sleepPeriod;
    int i, j, nRound, mainIndex;
    CArray <clock_t, clock_t> cStart;
    CArray <clock_t, clock_t> cPrevStart;
    cStart.SetSize(SMALL_NODE_SUM);
    cPrevStart.SetSize(SMALL_NODE_SUM);

    bool allSleep = true;


    CCommunicationController* mainController = (CCommunicationController*)pParam;

    // ------- SETUP THE CONNECTIONS ------------
    mainController->SetupSerialConnections();

    // --------------- RUNNING ----------------
    nRound = 0;

    CSerialControllerWithTx *cable = NULL;
    while (1)
    {
        for (j = 0; j < mainController->m_totalSerialConnection; j++)
        {
            i = j;

            // mainIndex is the connection-ID (instrument-number) that we're currently at...
            mainIndex = mainController->m_serialList[i]->m_mainIndex;

            // Get a handle to the serial-controller...
            cable = mainController->m_SerialControllerTx[i];

            // If we're using a radio then make sure we've set the radio-ID
            if (mainController->m_serialList[i]->m_medium == MEDIUM_FREEWAVE_SERIAL_MODEM)
                cable->SetModem(g_settings.scanner[mainIndex].comm.radioID);

            // Check wheather the scanning instrument is sleeping/should be sleeping
            cStart.SetAtGrow(i, clock());
            sleepPeriod = GetSleepTime(g_settings.scanner[mainIndex].comm.sleepTime,
                g_settings.scanner[mainIndex].comm.wakeupTime);

            // Check if we should go to sleep or not...
            if (sleepPeriod > 0)
            {
                // Make the instrument go to sleep...
                if (mainController->m_SerialControllerTx[i]->GoToSleep())
                {
                    nRound = 0;
                    mainController->m_serialList[i]->m_sleepFlag = true;
                }
                continue; // continue with the next instrument
            }
            else
            {
                if (mainController->m_serialList[i]->m_sleepFlag)
                {
                    cable->WakeUp();
                    mainController->m_nodeControl->SetNodeStatus(i, DeviceMode::Run, g_settings.outputDirectory);	//SET node status to run
                    mainController->m_serialList[i]->m_sleepFlag = false;
                }
            }

            // Ok, we're not sleeping. check if we should upload something to the
            //  instrument...
            if (mainController->m_nodeControl->GetNodeStatus(i) == DeviceMode::Special)
            {
                UploadFile_SerialTx(i, mainController, cable);
            }

            // Download data...
            cable->Start();

            nRound++;

            if (nRound > 0 && mainController->m_nodeControl->GetNodeStatus(i) != DeviceMode::Special)
            {
                //calculate pause time and pause
                Pause((double)cPrevStart[i], (double)cStart[i], i);
            }
            cPrevStart.SetAtGrow(i, cStart[i]);
        }// end for

        // Check if all instruments are sleeping.
        allSleep = true;
        for (i = 0; i < mainController->m_totalSerialConnection; i++)
        {
            allSleep = allSleep && mainController->m_serialList[i]->m_sleepFlag;
        }
        if (allSleep)
        {
            mainController->SleepAllNodes(SERIAL_CONNECTION);
        }

        // Go back and check all the instruments again...
    }
    return 0;
}

void Pause(double startTime, double stopTime, int serialID)
{
    double elapsedTime;
    long interval;
    CString msg;

    elapsedTime = (stopTime - startTime) / (double)CLOCKS_PER_SEC;
    interval = g_settings.scanner[serialID].comm.queryPeriod;
    if ((elapsedTime < interval) && (elapsedTime >= 0))
    {
        msg.Format("<node %d>:Will sleep %.1lf seconds", serialID, interval - elapsedTime);
        ShowMessage(msg);
        if ((interval - elapsedTime) > 0)
            Sleep((int)(interval - elapsedTime) * 1000);
    }
}
void CCommunicationController::SleepAllNodes(int connectionType)
{
    CString msg;
    long allSleepPeriod;
    int i;
    int sleepPeriodArray[MAX_NUMBER_OF_SCANNING_INSTRUMENTS];

    int connectionNumber[3] = { m_totalSerialConnection,m_totalFTPConnection,
                                                        m_totalHttpConnection };

    //assume all scanners sleep period is same
    allSleepPeriod = GetSleepTime(g_settings.scanner[0].comm.sleepTime,
        g_settings.scanner[0].comm.wakeupTime);
    for (i = 0; i < connectionNumber[connectionType]; i++)
    {
        sleepPeriodArray[i] = GetSleepTime(g_settings.scanner[i].comm.sleepTime,
            g_settings.scanner[i].comm.wakeupTime);

        if ((sleepPeriodArray[i] < allSleepPeriod) && (sleepPeriodArray[i] > 0))
            allSleepPeriod = sleepPeriodArray[i];
    }
    if (allSleepPeriod > 0)
    {
        msg.Format("All instruments sleep for %.2lf hours. The scanner will start working at %02d:%02d:%02d ", allSleepPeriod / 3600000.0,
            g_settings.scanner[0].comm.wakeupTime.hour,
            g_settings.scanner[0].comm.wakeupTime.minute,
            g_settings.scanner[0].comm.wakeupTime.second);
        ShowMessage(msg);

        Sleep(allSleepPeriod);
    }
}

void CCommunicationController::SetupSerialConnections()
{
    for (int i = 0; i < m_totalSerialConnection; i++)
    {
        int mainIndex = m_serialList.GetAt(i)->m_mainIndex;

        m_SerialControllerTx.SetAtGrow(i, new CSerialControllerWithTx(g_settings.scanner[mainIndex].electronicsBox));

        m_SerialControllerTx[i]->SetSerialPort(mainIndex,
            g_settings.scanner[mainIndex].comm.port,
            g_settings.scanner[mainIndex].comm.baudrate,
            NOPARITY,
            8,
            ONESTOPBIT,
            g_settings.scanner[mainIndex].comm.flowControl);


        // Show the connection to the user
        CString msg;
        msg.Format("<Serial%d>: COM%d, baudrate %dbps,flowControl %d.", i,
            g_settings.scanner[mainIndex].comm.port,
            g_settings.scanner[mainIndex].comm.baudrate,
            g_settings.scanner[mainIndex].comm.flowControl);
        ShowMessage(msg);

        if (g_settings.scanner[mainIndex].comm.medium == MEDIUM_FREEWAVE_SERIAL_MODEM)
            m_SerialControllerTx[i]->SetModem(g_settings.scanner[mainIndex].comm.radioID);

        m_SerialControllerTx[i]->WakeUp(0);
        Sleep(1000);
    }
}
//--------------End of CCommunicationController --------//

