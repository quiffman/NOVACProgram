#include "stdafx.h"
#include "../NovacMasterProgram.h"
#include "ConfigurationDlg.h"
#include "../Configuration/ConfigurationFileHandler.h"

using namespace ConfigurationDialog;
using namespace FileHandler;

// CConfigurationDlg dialog

IMPLEMENT_DYNAMIC(CConfigurationDlg, CDialog)
CConfigurationDlg::CConfigurationDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CConfigurationDlg::IDD, pParent)
{
}

CConfigurationDlg::~CConfigurationDlg()
{
}

void CConfigurationDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_STATIC_FRAME, m_frame);
}


BEGIN_MESSAGE_MAP(CConfigurationDlg, CDialog)
END_MESSAGE_MAP()


// CConfigurationDlg message handlers

BOOL CConfigurationDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// read the configuration file
	CConfigurationFileHandler reader;
	reader.ReadConfigurationFile(m_configuration);

	// now show the main page
	CRect rect;
	int margin = 20;
	this->m_frame.GetWindowRect(rect);
	int width = rect.right - rect.left - margin;
	int height = rect.bottom - rect.top - margin;

	// construct the sheet
	m_sheet.Construct("", this);

	// the global configuration
	m_pageGlobal.Construct(IDD_CONFIGURATION_GLOBAL);
	m_pageGlobal.m_pPSP->dwFlags |= PSP_PREMATURE;
	m_pageGlobal.m_configuration = &m_configuration;

	// the scanner-specific configuration
	m_pageScanner.Construct(IDD_CONFIGURATION_SCANNER);
	m_pageScanner.m_pPSP->dwFlags |= PSP_PREMATURE;
	m_pageScanner.m_configuration = &m_configuration;

	// add the pages to the sheet
	m_sheet.AddPage(&m_pageScanner);
	m_sheet.AddPage(&m_pageGlobal);

	m_sheet.Create(this, WS_CHILD | WS_VISIBLE | WS_TABSTOP);
	m_sheet.ModifyStyleEx(0, WS_EX_CONTROLPARENT);
	m_sheet.MoveWindow(margin, margin, width, height, TRUE);

	return TRUE;  // return TRUE unless you set the focus to a control
	// EXCEPTION: OCX Property Pages should return FALSE
}

void ConfigurationDialog::CConfigurationDlg::OnOK()
{
	// First check the settings
	if(SUCCESS != CheckSettings()){
		return;
	}

	// Write the configuration file
	CConfigurationFileHandler writer;
	writer.WriteConfigurationFile(m_configuration);

	// Tell the user that the program needs to be restarted
	MessageBox("Settings saved. You need to restart the program for settings to come into effect", "RESTART!", MB_OK);

	// exit the dialog
	CDialog::OnOK();
}


RETURN_CODE ConfigurationDialog::CConfigurationDlg::CheckSettings(){
	unsigned int i, j, c;
	int k;
	CString message;

	for(i = 0; i < m_configuration.scannerNum; ++i){
		for(j = 0; j < m_configuration.scanner[i].specNum; ++j){
			bool pass = true;

			// Check the names
			if(strlen(m_configuration.scanner[i].observatory) == 0){
				message.Format("Please supply a name for the observatory");
				pass = false;
			}
			if(strlen(m_configuration.scanner[i].volcano) == 0){
				message.Format("Please supply a name for the volcano");
				pass = false;
			}
			if(strlen(m_configuration.scanner[i].site) == 0){
				message.Format("Please supply a name for the site of the scanner");
				pass = false;
			}

			for(c = 0; c < m_configuration.scanner[i].spec[j].channelNum; ++ c){ 
				// Check the fit window
				Evaluation::CFitWindow &window = m_configuration.scanner[i].spec[j].channel[c].fitWindow;

				if(window.fitHigh <= window.fitLow){
					message.Format("FitLow must be lower than FitHigh");
					pass = false;
				}

				if(window.polyOrder <= 0 || window.polyOrder > 5){
					message.Format("The order of the fitted polynomial must be an integer between 0 and 5.");
					pass = false;
				}

				if(window.nRef == 0){
					message.Format("No reference files are defined for the evaluation.");
					pass = false;
				}

				for(k = 0; k < window.nRef; ++k){
					FILE *f = fopen(window.ref[k].m_path.c_str(), "r");
					if(f == NULL){
						message.Format("Cannot read reference file %s", window.ref[k].m_path.c_str());
						pass = false;
						break;
					}
					fclose(f);
				}
			}

			if(!pass){
				CString str;
				str.Format("Please check settings for spectrometer %s. ", (LPCSTR)m_configuration.scanner[i].spec[j].serialNumber);
				str.AppendFormat(message);
				MessageBox(str, "Configuration Error");
				return FAIL;
			}

		}
	}

	// Check if the user has specified a script to excecute at the arrival of each scan
	if(strlen(m_configuration.externalSetting.fullScanScript) > 1){
		if(!IsExistingFile(m_configuration.externalSetting.fullScanScript)){
			MessageBox("Specified script to excecute at receival of scans cannot be found.");
			return FAIL;
		}
	}

	// nothing to complain about
	return SUCCESS;
}
