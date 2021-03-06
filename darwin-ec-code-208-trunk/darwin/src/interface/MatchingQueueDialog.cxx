//*******************************************************************
//   file: MatchingQueueDialog.cxx
//
// author: Adam Russell
//
//   mods: J H Stewman (1/24/2008)
//         -- comment blocks added
//
//*******************************************************************

#ifdef HAVE_CONFIG_H
	#include <config.h>
#endif

#include <string.h>
#include "../support.h"
#include "MatchingQueueDialog.h"
#include "MatchResultsWindow.h"
#include "../image_processing/transform.h"
//#include "ErrorDialog.h"

#ifdef WIN32
#define PATH_SLASH "\\"
#else
#define PATH_SLASH "/"
#endif

using namespace std;

static const int IMAGE_WIDTH = 300;
static const int IMAGE_HEIGHT = 300;

static string gLastDirectory = "";   // last path in use
static string gLastFileName = "";

static gchar  *gLastTreePathStr = NULL; // last GtkTree path (ex:"10:3:5") -- must free with g_free()
static GtkTreePath *gLastTreePath;

static int gNumReferences = 0;

//*******************************************************************
//
// a LOCAL utility used to copy matchQResults files to "old" folder
// and remove same from "matchQResults" in preparation for running
// a new match
//
//***1.85 - revised to work on a single result file each time called
//
void backupAndRemoveMatchQResults(string filename)
{
	char path[1024];
	//***1.85 - match results now go inside current survey area
	sprintf(path, "%s%smatchQResults%s", 
		gOptions->mCurrentSurveyArea.c_str(), PATH_SLASH, PATH_SLASH);
	string command = "";

#ifdef WIN32
	command = ((((command + "COPY /Y /V \"") + filename) + "\" \"") + path) + "old\" >nul";
#else
	command = ((((command + "cp \"") + filename) + "\" \"") + path) + "old\"";
#endif
	// back-up current results, if any, overwriting existing older backup
	system(command.c_str());

	command = "";
#ifdef WIN32
	command = ((command + "DEL /Q \"") + filename) + "\" >nul";
#else
	command = ((command + "rm \"") + filename) + "\"";
#endif
	// wipe current results, so no conflict with results about to be written
	system(command.c_str());
}

//*******************************************************************
//
int getNumMatchingQueueDialogReferences()
{
	return gNumReferences;
}

//*******************************************************************
//
MatchingQueueDialog::MatchingQueueDialog(
		MainWindow *mainWin,
		Database *db, 
		Options *o)
	:
		mDialog(NULL),
		mCList(NULL),
		mDrawingArea(NULL),
		//mFileSelectionDialog(NULL),
		mFileChooserDialog(NULL), //***1.4
		mMatchingQueue(NULL),
		mImage(NULL),
		mLastRowSelected(-1),
		mMatchCancelled(false),
		mMatchRunning(false),
		mMatchPaused(false),
		mFinDatabase(db),
		mMainWin(mainWin),
		mOptions(o)        //***054
{
	if (NULL == db)
		throw Error("Empty database in MatchingQueueDialog constructor.");

	mDialog = createMatchingQueueDialog();
		
	mMatchingQueue = new MatchingQueue(db,o);

	gNumReferences++;
}

//*******************************************************************
//
MatchingQueueDialog::~MatchingQueueDialog()
{
	if (NULL != mDialog)
	{
		gtk_widget_destroy(GTK_WIDGET(mDialog));
		mDialog = NULL;
	}

	if (NULL != mImage)
	{
		delete mImage;
		mImage = NULL;
	}

	if (NULL != mMatchingQueue)
	{
		delete mMatchingQueue;
		mMatchingQueue = NULL;
	}

	gNumReferences--;
}

//*******************************************************************
//
void MatchingQueueDialog::show()
{
	gtk_widget_show(mDialog);
	updateQueueList();

	// reset bools just in case we returned to this window from somewhere else

	mMatchCancelled = false;
	mMatchRunning = false;
	mMatchPaused = false;

	// NOTE: the idle function is started in the create...() function rather than here
	// because it is not stopped (removed) when a MatchResultsWindow is created.
	// This differs from how it is done in the MatchingDialog.  It allows multiple
	// MatchResults to be displayed simultaneously here.
}

//*******************************************************************
//
// gboolean matchingQueueIdleFunction(...)
//
//    Idle CALLBACK.  This drives the matching process.  Actions are
//    driven by values of several booleans and the selected catalog
//    categories.
//
gboolean matchingQueueIdleFunction(
	gpointer userData)
{
	MatchingQueueDialog *dlg = (MatchingQueueDialog *)userData;

	if (NULL == dlg)
		return FALSE; // stop coming back to idle function

	if (dlg->mMatchCancelled) {
		// if the match has been cancelled, we end up here for cleanup
		// there is no need to explicitly delete any of the "pointed to" items
		// here.  the delete dlg causes all to be deleted in due time.
		delete dlg;
		return FALSE;
	}

	if (dlg->mMatchPaused || !dlg->mMatchRunning /*|| (dlg->mCategoriesSelected == 0)*/)
		return TRUE;  // do nothing, but keep coming back to this idle function

	cout << ".";

	// note: not quite sure what action to take when STOP is requested,
	// probably should be to reinitialize the match and be ready to start over
	// for now treat STOP the same as PAUSE

	try {

		// if whole database has been matched against current unknown, then
		// move on to next unknown and reset database index
		// otherwise, move on to next database fin and match it to current unknown

		Match *matcher;
		char fName[500];

		if (-1 == /*id*/dlg->mLastRowSelected) // get the first unknown
		{
			matcher = dlg->mMatchingQueue->getNextUnknownToMatch();

			dlg->mLastRowSelected++;

			// if matcher is NULL it indicates a problem loading an uknown .fin or .finz
			if (NULL == matcher)
				cout << "Skipping row " << dlg->mLastRowSelected << "!";
			else
				// move down queue list and display image of unknown fin  being matched
				gtk_clist_select_row(GTK_CLIST(dlg->mCList), dlg->mLastRowSelected, 0);
		}
		else // just reset the pointer to the current unknown
			matcher = dlg->mMatchingQueue->getCurrentUnknownToMatch();

		if (NULL != matcher)
		{
		bool categoriesToMatch[32] = 
				{true,true,true,true,true,true,true,true,
				 true,true,true,true,true,true,true,true,
				 true,true,true,true,true,true,true,true,
				 true,true,true,true,true,true,true,true};

		// matcher should NEVER be NULL here, because the percentComplete below
		// will reach 1.0 first and terminate returns to this idle function,
		// but test just in case

		float percentDatabaseProcessed = matcher->matchSingleFin(
				TRIM_OPTIMAL_TIP,
				ALL_POINTS,
				categoriesToMatch,
				false, // use traling edge only in final error (true == use full outline)
				true); // use absolute offsets to access database fins

		gtk_progress_set_value(
				GTK_PROGRESS(dlg->mProgressBar2),
				percentDatabaseProcessed * 100);

		if (percentDatabaseProcessed < 1.0)
			return TRUE;

		// at this point percentDatabaseProcessed == 1.0, so
		// this unknown has been matched to entire database, so save results

		gchar *finFileName;
		gtk_clist_get_text(GTK_CLIST (dlg->mCList),dlg->mLastRowSelected,0,&finFileName);
		string finFileRoot = finFileName;
		finFileRoot = finFileRoot.substr(0,finFileRoot.rfind('.')); // strip .fin or .finz

		//***1.9 - break out database name to make part of results filename
		string dbName = dlg->mOptions->mDatabaseFileName;
		dbName = dbName.substr(dbName.rfind(PATH_SLASH)+1);
		dbName = dbName.substr(0,dbName.rfind(".db"));

		//***1.85 - match results now go inside current survey area folder
		//***1.9 - match results now include area and database in filename
		sprintf(fName, "%s%smatchQResults%s%s-DB-match-for-%s.res", 
				dlg->mOptions->mCurrentSurveyArea.c_str(), PATH_SLASH, PATH_SLASH, 
				dbName.c_str(), finFileRoot.c_str());
		backupAndRemoveMatchQResults(fName); //***1.85
		cout << "\nSaving OLD results and creating new results file ...\n";
		cout << "  " << fName << endl;
		dlg->mMatchingQueue->getMatchResults()->sort(); //***1.5 - list must be sorted here, not as built
		dlg->mMatchingQueue->getMatchResults()->save(fName);
		dlg->mMatchingQueue->finalizeMatch();
		}

		// now get new unknown so matching can be continued
		// NOTE: the current (*matcher) will be deleted by the call to
		// getNextUnknownToMatch(), so don't do it here

		matcher = dlg->mMatchingQueue->getNextUnknownToMatch();
		
		dlg->mLastRowSelected++;

		// if matcher is NULL it indicates a problem loading an uknown .fin or .finz
		if (NULL == matcher)
			cout << "Skipping row " << dlg->mLastRowSelected << "!";
		else
			// move down queue list and display image of unknown fin  being matched
			gtk_clist_select_row(GTK_CLIST(dlg->mCList), /*id*/dlg->mLastRowSelected, 0);

		float percentComplete = dlg->mMatchingQueue->matchProgress();

		gtk_progress_set_value(
				GTK_PROGRESS(dlg->mProgressBar1),
				percentComplete * 100);

		if (percentComplete < 1.0)
			return TRUE;

		// the matching is done so summarize results & stop returning to idle function

		dlg->mMatchingQueue->summarizeMatching(); // output to console

		//***1.85 - match results are now inside current survey area
		sprintf(fName, "%s%smatchQResults%sresults-summary", 
				gOptions->mCurrentSurveyArea.c_str(), PATH_SLASH, PATH_SLASH);
		ofstream outFile(fName);
		if (! outFile.fail())
		{
			dlg->mMatchingQueue->summarizeMatching(outFile); // output to file
			outFile.close();
		}

		delete dlg;

		return FALSE;

	} catch (Error e) {
		showError(e.errorString());
		delete dlg;
		return FALSE;
	}
}


//*******************************************************************
//
GtkWidget* MatchingQueueDialog::createMatchingQueueDialog()
{
	GtkWidget *matchingQueueDialog;
	GtkWidget *dialog_vbox1;
	GtkWidget *matchingQueueVBox;
	GtkWidget *matchingQueueHBox;
	GtkWidget *matchingQueueScrolledWindow;
	GtkWidget *matchingQueueLabelFilename;
	//GtkWidget *matchingQueueLabelDate;
	//GtkWidget *matchingQueueLabelLocation;
	GtkWidget *dialog_action_area1;
	GtkWidget *hbuttonbox3;
	GtkWidget *matchingQueueHButtonBox;
	GtkWidget *matchingQueueButtonAdd;
	GtkWidget *matchingQueueButtonRemove;
	GtkWidget *matchingQueueButtonRunMatch;
	GtkWidget *matchingQueueButtonViewResults;
	GtkWidget *matchingQueueButtonSaveList;
	GtkWidget *matchingQueueButtonLoadList;
	GtkWidget *matchingQueueButtonCancel;

	matchingQueueDialog = gtk_dialog_new ();
	gtk_object_set_data (GTK_OBJECT (matchingQueueDialog), "matchingQueueDialog", matchingQueueDialog);
	gtk_window_set_title (GTK_WINDOW (matchingQueueDialog), _("Matching Queue"));
	//GTK_WINDOW (matchingQueueDialog)->type = WINDOW_DIALOG;
	gtk_window_set_policy (GTK_WINDOW (matchingQueueDialog), TRUE, TRUE, TRUE);
	gtk_window_set_position(GTK_WINDOW(matchingQueueDialog), GTK_WIN_POS_CENTER); //***1.8
	gtk_window_set_wmclass(GTK_WINDOW(matchingQueueDialog), "darwin_matchingqueue", "DARWIN");

	/*gtk_window_set_modal(GTK_WINDOW(matchingQueueDialog), TRUE);
	gtk_window_set_transient_for(
			GTK_WINDOW(matchingQueueDialog),
			GTK_WINDOW(mMainWin->getWindow()));
*/
	dialog_vbox1 = GTK_DIALOG (matchingQueueDialog)->vbox;
	gtk_object_set_data (GTK_OBJECT (matchingQueueDialog), "dialog_vbox1", dialog_vbox1);
	gtk_widget_show (dialog_vbox1);

	matchingQueueHBox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (matchingQueueHBox);
	gtk_box_pack_start (GTK_BOX (dialog_vbox1), matchingQueueHBox, TRUE, TRUE, 0);

	matchingQueueVBox = gtk_vbox_new(FALSE, 0);
	gtk_widget_show(matchingQueueVBox);
	gtk_box_pack_start(GTK_BOX(matchingQueueHBox), matchingQueueVBox, TRUE, TRUE, 0);

	matchingQueueScrolledWindow = gtk_scrolled_window_new (NULL, NULL);
	gtk_widget_show (matchingQueueScrolledWindow);
	gtk_box_pack_start (GTK_BOX (matchingQueueVBox), matchingQueueScrolledWindow, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (matchingQueueScrolledWindow), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	matchingQueueHButtonBox = gtk_hbutton_box_new();
	gtk_widget_show(matchingQueueHButtonBox);
	gtk_box_pack_start(GTK_BOX(matchingQueueVBox), matchingQueueHButtonBox, FALSE, FALSE, 0);

	matchingQueueButtonAdd = gtk_button_new_with_label (_("Add..."));
	gtk_widget_show(matchingQueueButtonAdd);
	gtk_container_add(GTK_CONTAINER (matchingQueueHButtonBox), matchingQueueButtonAdd);
	GTK_WIDGET_SET_FLAGS(matchingQueueButtonAdd, GTK_CAN_DEFAULT);

	matchingQueueButtonRemove = gtk_button_new_with_label(_("Remove"));
	gtk_widget_show(matchingQueueButtonRemove);
	gtk_container_add(GTK_CONTAINER (matchingQueueHButtonBox), matchingQueueButtonRemove);
	GTK_WIDGET_SET_FLAGS(matchingQueueButtonRemove, GTK_CAN_DEFAULT);

	mCList = gtk_clist_new (3);
	gtk_widget_show (mCList);
	gtk_container_add (GTK_CONTAINER (matchingQueueScrolledWindow), mCList);

	for (int i = 0; i < 3; i++)
		gtk_clist_set_column_auto_resize(GTK_CLIST(mCList), i, TRUE);
	gtk_clist_column_titles_show (GTK_CLIST (mCList));

	matchingQueueLabelFilename = gtk_label_new (_("Traced Fins"));
	gtk_widget_show (matchingQueueLabelFilename);
	gtk_clist_set_column_widget (GTK_CLIST (mCList), 0, matchingQueueLabelFilename);

    //***1.1 - create vertically packed box in right pane

	GtkWidget *matchingVBox = gtk_vbox_new(FALSE, 0);
	gtk_object_set_data (GTK_OBJECT (matchingQueueDialog), "matchingVBox", matchingVBox);
	gtk_container_add(GTK_CONTAINER(matchingQueueHBox), matchingVBox);
	//gtk_box_pack_start(GTK_BOX(matchingQueueHBox), matchingVBox, TRUE, TRUE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (matchingVBox), 4);
	gtk_widget_show(matchingVBox);

	mDrawingArea = gtk_drawing_area_new ();
	gtk_drawing_area_size(
			GTK_DRAWING_AREA(mDrawingArea),
			IMAGE_WIDTH,
			IMAGE_HEIGHT
			);
	gtk_widget_show (mDrawingArea);
	gtk_box_pack_start (GTK_BOX (matchingVBox), mDrawingArea, TRUE, TRUE, 0);

	//***1.1 - added progress bars in 


  	// Place label "Percent of Queue Processed:" above the sliding progress bar

	GtkWidget *matchingLabel = gtk_label_new (_("Percent of Queue Processed:"));
	gtk_widget_show (matchingLabel);
	gtk_box_pack_start (GTK_BOX (matchingVBox), matchingLabel, FALSE, FALSE, 6);

	// Place the progress bar below the label

	mProgressBar1 = gtk_progress_bar_new ();
	gtk_widget_show (mProgressBar1);
	gtk_box_pack_start (GTK_BOX (matchingVBox), mProgressBar1, FALSE, FALSE, 0);
	gtk_progress_set_show_text (GTK_PROGRESS (mProgressBar1), TRUE);

  	// Place label "Percent of Database Matched\n Against Current Unknown:" above the sliding progress bar

	GtkWidget *matchingLabel2 = gtk_label_new (
			_("Percent of Database Matched\n Against Current Unknown:"));
	gtk_widget_show (matchingLabel2);
	gtk_box_pack_start (GTK_BOX (matchingVBox), matchingLabel2, FALSE, FALSE, 6);

	// Place the progress bar below the label

	mProgressBar2 = gtk_progress_bar_new ();
	gtk_widget_show (mProgressBar2);
	gtk_box_pack_start (GTK_BOX (matchingVBox), mProgressBar2, FALSE, FALSE, 0);
	gtk_progress_set_show_text (GTK_PROGRESS (mProgressBar2), TRUE);

	dialog_action_area1 = GTK_DIALOG (matchingQueueDialog)->action_area;
	gtk_object_set_data (GTK_OBJECT (matchingQueueDialog), "dialog_action_area1", dialog_action_area1);
	gtk_widget_show (dialog_action_area1);
	gtk_container_set_border_width (GTK_CONTAINER (dialog_action_area1), 10);

	hbuttonbox3 = gtk_hbutton_box_new ();
	gtk_widget_show (hbuttonbox3);
	gtk_box_pack_start (GTK_BOX (dialog_action_area1), hbuttonbox3, TRUE, TRUE, 0);

	matchingQueueButtonRunMatch = gtk_button_new_with_label (_("Run Match"));
	gtk_widget_show (matchingQueueButtonRunMatch);
	gtk_container_add (GTK_CONTAINER (hbuttonbox3), matchingQueueButtonRunMatch);
	GTK_WIDGET_SET_FLAGS (matchingQueueButtonRunMatch, GTK_CAN_DEFAULT);

	matchingQueueButtonViewResults = gtk_button_new_with_label (_("View Results"));
	gtk_widget_show (matchingQueueButtonViewResults);
	gtk_container_add (GTK_CONTAINER (hbuttonbox3), matchingQueueButtonViewResults);
	GTK_WIDGET_SET_FLAGS (matchingQueueButtonViewResults, GTK_CAN_DEFAULT);

	matchingQueueButtonSaveList = gtk_button_new_with_label (_("Save Queue"));
	gtk_widget_show (matchingQueueButtonSaveList);
	gtk_container_add (GTK_CONTAINER (hbuttonbox3), matchingQueueButtonSaveList);
	GTK_WIDGET_SET_FLAGS (matchingQueueButtonSaveList, GTK_CAN_DEFAULT);

	matchingQueueButtonLoadList = gtk_button_new_with_label (_("Load Queue"));
	gtk_widget_show (matchingQueueButtonLoadList);
	gtk_container_add (GTK_CONTAINER (hbuttonbox3), matchingQueueButtonLoadList);
	GTK_WIDGET_SET_FLAGS (matchingQueueButtonLoadList, GTK_CAN_DEFAULT);

	matchingQueueButtonCancel = gtk_button_new_with_label (_("Cancel"));
	gtk_widget_show (matchingQueueButtonCancel);
	gtk_container_add (GTK_CONTAINER (hbuttonbox3), matchingQueueButtonCancel);
	GTK_WIDGET_SET_FLAGS (matchingQueueButtonCancel, GTK_CAN_DEFAULT);
	
	gtk_signal_connect (GTK_OBJECT (matchingQueueDialog), "delete_event",
					GTK_SIGNAL_FUNC (on_matchingQueueDialog_delete_event),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (mCList), "select_row",
					GTK_SIGNAL_FUNC (on_MQ_mCList_select_row),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (mDrawingArea), "expose_event",
					GTK_SIGNAL_FUNC (on_matchingQueueDrawingArea_expose_event),
					(void*)this);

	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonAdd), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonAdd_clicked),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonRemove), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonRemove_clicked),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonRunMatch), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonRunMatch_clicked),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonViewResults), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonViewResults_clicked),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonSaveList), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonSaveList_clicked),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonLoadList), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonLoadList_clicked),
					(void*)this);
	gtk_signal_connect (GTK_OBJECT (matchingQueueButtonCancel), "clicked",
					GTK_SIGNAL_FUNC (on_matchingQueueButtonCancel_clicked),
					(void*)this);

	gtk_widget_grab_default (matchingQueueButtonRunMatch);

	gtk_idle_add(matchingQueueIdleFunction, (void *) this);

	return matchingQueueDialog;
}

//*******************************************************************
//
void MatchingQueueDialog::updateQueueList()
{
	cout << "updating queue list" << endl;
	if (NULL == mCList)
		return;

	gtk_clist_freeze(GTK_CLIST(mCList));
	gtk_clist_clear(GTK_CLIST(mCList));
	
	cout << "getting queue" << endl;
	list<queueItem_t> qList = mMatchingQueue->getQueue();
	list<queueItem_t>::iterator it = qList.begin();

	cout << "iterating list" << endl;
	while (it != qList.end()) {
		gchar *name, *date, *location;
		
		cout << "filename " << it->fileName << endl;
		name = new gchar[it->fileName.length() + 1];
		strcpy(name, it->fileName.c_str());
		
		cout << "date " << it->date <<endl;
		date = new gchar[it->date.length() + 1];
		strcpy(date, it->date.c_str());
		
		cout << "location " << it->location << endl;
		location = new gchar[it->location.length() + 1];
		strcpy(location, it->location.c_str());

		gchar *itemInfo[3] = {
			name,
			date,
			location
		};

		gtk_clist_append(GTK_CLIST(mCList), itemInfo);

		delete[] name;
		delete[] date;
		delete[] location;

		++it;
	}

	cout << "switch action" << endl;

	//***1.2 - appropriately select and highlight item in list
	switch (this->mActionSelected)
	{
	case ADD_FILENAME :
		// new filename is always added at the end of the list
		cout << "add filename" << endl;
		mLastRowSelected = qList.size() - 1;
		cout << "set selected row" << endl;
		if (mLastRowSelected >= 0)
		{
			cout << "clist select row " << mLastRowSelected << endl;
			gtk_clist_select_row(GTK_CLIST(mCList), mLastRowSelected, 0);
		}
		break;
	case DELETE_FILENAME :
		// same relative position in list is seleceted, unless last item
		// was removed.  in latter case we back up one position.
		if (mLastRowSelected >= qList.size())
			mLastRowSelected--;
		if (mLastRowSelected >= 0)
			gtk_clist_select_row(GTK_CLIST(mCList), mLastRowSelected, 0);
		break;
	case LOAD_QUEUE :
		// new queue loaded, so select first item in the list
		if (qList.size() <=0)
			mLastRowSelected = -1;
		else
			mLastRowSelected = 0;
		if (mLastRowSelected >= 0)
			gtk_clist_select_row(GTK_CLIST(mCList), mLastRowSelected, 0);
		break;
	case SAVE_QUEUE :
		// no action required, list is unchanged
		break;
	case VIEW_RESULTS :
		// no action required, list is unchanged
		break;
	default :
		// no action required, list is unchanged
		break;
	}

	cout << "gtk_clist_thaw" << endl;

	gtk_clist_thaw(GTK_CLIST(mCList));
}

//*******************************************************************
//
gboolean on_matchingQueueDialog_delete_event(
	GtkWidget *widget,
	GdkEvent *event,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;

	//delete dialog; this will be done by idle function
	dialog->mMatchCancelled = true; //***1.1 - this will cause window close with correct cleanup

	return TRUE;
}

//*******************************************************************
//
void on_MQ_mCList_select_row(
	GtkCList *clist,
	gint row,
	gint column,
	GdkEvent *event,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;
	
	if (NULL == dialog)
		return;

	dialog->mLastRowSelected = row;

	try {

		string finFileName = dialog->mMatchingQueue->getItemNum(row); //***1.1
		//DatabaseFin<ColorImage> *fin = dialog->mMatchingQueue->getItemNum(row);
		DatabaseFin<ColorImage> *fin = NULL;

		cout << "filename " << finFileName << endl;
		
		cout << "found finz: " << finFileName.rfind(".finz") << endl;
		

		if(finFileName.rfind(".finz") != string::npos)
		{
			cout << "opening finz" << endl;
			fin = openFinz(finFileName);
			//CHECK 
			if (fin==NULL) {
				//g_print("Bad fin...");
				//dialog->mMatchingQueue->remove(dialog->mLastRowSelected);
				//dialog->updateQueueList();
				//return;
				///throw(Error("Fin File missing or corrupted!"));
			}


		}
		else
		{
			cout << "opening dbfin" << endl;
			if (isTracedFinFile(finFileName))
				fin = new DatabaseFin<ColorImage>(finFileName); //***1.1
		}

		if (NULL == fin)
			throw(Error("Fin File missing or corrupted!"));

		if (NULL != dialog->mImage)
			delete dialog->mImage;
		
		cout << "opening new color image" << endl;
		ColorImage *newImage = new ColorImage(fin->mImageFilename); //***1.1
		
		cout << "setting resized image" << endl;
		dialog->mImage = resizeWithBorder(/*dbFin->mFinImage*/newImage, IMAGE_HEIGHT, IMAGE_WIDTH);
		on_matchingQueueDrawingArea_expose_event(
				dialog->mDrawingArea,
				NULL,
				(void*)dialog
				);
		
		cout << "deleting newImage and fin" << endl;
		delete newImage; //***1.1
		delete fin;

	} catch (Error e) {
		showError(e.errorString());
	}
}

//*******************************************************************
//
gboolean on_matchingQueueDrawingArea_expose_event(
	GtkWidget *widget,
	GdkEventExpose *event,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;
	
	if (NULL == dialog)
		return FALSE;
	
	if (NULL == dialog->mImage)
		return FALSE;

	gdk_draw_rgb_image(
		widget->window,
		widget->style->fg_gc[GTK_STATE_NORMAL],
		0, 0,
		dialog->mImage->getNumCols(),
		dialog->mImage->getNumRows(),
		GDK_RGB_DITHER_NONE,
		(guchar*)dialog->mImage->getData(),
		dialog->mImage->getNumCols() * dialog->mImage->bytesPerPixel());
	
	return TRUE;
}

//*******************************************************************
//
void on_matchingQueueButtonAdd_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;
	
	if (NULL == dialog)
		return;
	
//	if (NULL != dialog->mFileSelectionDialog)
//		return;

	if (NULL != dialog->mFileChooserDialog) //***1.4
		return; //***1.4
	
	dialog->mActionSelected = ADD_FILENAME;

	//***1.4 - begin new code for use of file chooser so multiple selection works correctly
	dialog->mFileChooserDialog = dialog->createMatchingQueueFileChooserDialog();
	mqFileChooser_run_and_respond(dialog);
}

//*******************************************************************
//
void on_matchingQueueButtonRemove_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;

	if (NULL == dialog)
		return;

	if (dialog->mLastRowSelected == -1)
		return;

	dialog->mActionSelected = DELETE_FILENAME;
	dialog->mMatchingQueue->remove(dialog->mLastRowSelected);
	dialog->updateQueueList();
}

//*******************************************************************
//
void on_matchingQueueButtonRunMatch_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;

	if (NULL == dialog)
		return;
	
	if (dialog->mMatchingQueue->queueIsEmpty())
	{	
		string msg = "Create a queue of traced fin filenames (*.fin) ...\n\n";
		msg += "or Load a previously saved queue (*.que) ...\n\n";
		msg += "BEFORE running a match!";
		showError(msg);
		return;
	}

	//***1.3 - clean up previous results prior to running new match queue
	//backupAndRemoveMatchQResults(); //***1.85 - now done on a file by file basis

	dialog->mMatchingQueue->setupMatching();
	dialog->mLastRowSelected = -1;
	dialog->mMatchRunning = true;
}

//*******************************************************************
//
void on_matchingQueueButtonViewResults_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;

	if (NULL == dialog)
		return;
	
	if (NULL != dialog->mFileChooserDialog) //***1.4
		return; //***1.4

	dialog->mActionSelected = VIEW_RESULTS;

	dialog->mFileChooserDialog = dialog->createMatchingQueueFileChooserDialog(); //***1.4
	mqFileChooser_run_and_respond(dialog); //***1.4
}

//*******************************************************************
//
void on_matchingQueueButtonSaveList_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;
	
	if (NULL == dialog)
		return;
	
	if (NULL != dialog->mFileChooserDialog) //***1.4
		return; //***1.4
	
	dialog->mActionSelected = SAVE_QUEUE;

	dialog->mFileChooserDialog = dialog->createMatchingQueueFileChooserDialog(); //***1.4
	mqFileChooser_run_and_respond(dialog); //***1.4
}

//*******************************************************************
//
void on_matchingQueueButtonLoadList_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;
	
	if (NULL == dialog)
		return;
	
	if (NULL != dialog->mFileChooserDialog) //***1.4
		return; //***1.4

	dialog->mActionSelected = LOAD_QUEUE;

	dialog->mFileChooserDialog = dialog->createMatchingQueueFileChooserDialog(); //***1.4
	mqFileChooser_run_and_respond(dialog); //***1.4
}

//*******************************************************************
//
void on_matchingQueueButtonCancel_clicked(
	GtkButton *button,
	gpointer userData
	)
{
	// this should stop current match and then allow, a different queue
	// to be matched 

	MatchingQueueDialog *dialog = (MatchingQueueDialog*)userData;

	dialog->mMatchCancelled = true;
}

//---------------------------------------- 1.4 - new file chooser code ------------------------------

//*******************************************************************
//
GtkWidget* MatchingQueueDialog::createMatchingQueueFileChooserDialog()
{
	GtkWidget *matchingQueueFileChooserDialog;
	//GtkWidget *mqFileChooserButtonOK;
	//GtkWidget *mqFileChooserButtonCancel;
	GtkFileFilter *filter;

	// set base path to %DARWINHOME%
	//string directory = getenv("DARWINHOME");
	//***1.85 - everything is now relative to the current survey area
	string directory = gOptions->mCurrentSurveyArea;
	directory += PATH_SLASH;

	switch (mActionSelected) 
	{
	case ADD_FILENAME:
  		matchingQueueFileChooserDialog = gtk_file_chooser_dialog_new (
				_("Select a Traced Fin (*.fin, *.finz)"),
				GTK_WINDOW(this->mDialog),
				GTK_FILE_CHOOSER_ACTION_OPEN,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
				NULL);
				
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "Fin(z) Files (*.fin, *.finz)");
		gtk_file_filter_add_pattern(filter, "*.fin");
		gtk_file_filter_add_pattern(filter, "*.finz");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(matchingQueueFileChooserDialog),filter);
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "All Files (*.*)");
		gtk_file_filter_add_pattern(filter, "*.*");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(matchingQueueFileChooserDialog),filter);

		directory += "tracedFins";
		// allow multiple file selections
		gtk_file_chooser_set_select_multiple (
				GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
				TRUE);
		
		// currently we force focus back on the "tracedFins" folder

		if (directory == gLastDirectory)
		{
			// go back to last selected file
			gtk_file_chooser_set_filename (
					GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
					((gLastDirectory+PATH_SLASH)+gLastFileName).c_str());
		}
		else
		{
			// return focus to "tracedFins" folder wih NO selected file
			gtk_file_chooser_set_current_folder (
					GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
					directory.c_str());
			gLastDirectory = directory;
			gLastFileName = "";
		}
		break;

	case LOAD_QUEUE:
		matchingQueueFileChooserDialog = gtk_file_chooser_dialog_new (
				_("Load Matching Queue (*.que)"),
				GTK_WINDOW(this->mDialog),
				GTK_FILE_CHOOSER_ACTION_OPEN,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
				NULL);

		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "Queue Files (*.que)");
		gtk_file_filter_add_pattern(filter, "*.que");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(matchingQueueFileChooserDialog),filter);
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "All Files (*.*)");
		gtk_file_filter_add_pattern(filter, "*.*");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(matchingQueueFileChooserDialog),filter);

		directory += "matchQueues";
		// prevent multiple file selections
		gtk_file_chooser_set_select_multiple (
				GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
				FALSE);

		// currently we force focus back on the "matchQueues" folder

		if (directory == gLastDirectory)
		{
			// go back to last selected file
			gtk_file_chooser_set_filename (
					GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
					((gLastDirectory+PATH_SLASH)+gLastFileName).c_str());
		}
		else
		{
			// return focus to "matchQueues" folder wih NO selected file
			gtk_file_chooser_set_current_folder (
					GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
					directory.c_str());
			gLastDirectory = directory;
			gLastFileName = "";
		}
		break;

	case SAVE_QUEUE:
		matchingQueueFileChooserDialog = gtk_file_chooser_dialog_new (
				_("Save Matching Queue As ... (*.que)"),
				GTK_WINDOW(this->mDialog),
				GTK_FILE_CHOOSER_ACTION_SAVE,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
				NULL);
		directory += "matchQueues";
		gtk_file_chooser_set_current_folder (
				GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
				directory.c_str());
		gtk_file_chooser_set_current_name (
				GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
				"Untitled.que");
		// prevent multiple file selections
		gtk_file_chooser_set_select_multiple (
				GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
				FALSE);
		break;

	case VIEW_RESULTS:
		matchingQueueFileChooserDialog = gtk_file_chooser_dialog_new (
				_("Select Results file ..."),
				GTK_WINDOW(this->mDialog),
				GTK_FILE_CHOOSER_ACTION_OPEN,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
				NULL);

		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "Queue Files (*.res)");
		gtk_file_filter_add_pattern(filter, "*.res");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(matchingQueueFileChooserDialog),filter);
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "All Files (*.*)");
		gtk_file_filter_add_pattern(filter, "*.*");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(matchingQueueFileChooserDialog),filter);

		directory += "matchQResults";
		//directory += PATH_SLASH;
		// prevent multiple file selections
		gtk_file_chooser_set_select_multiple (
				GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
				FALSE);

		// currently we force focus back on the "matchQResults" folder

		if (directory == gLastDirectory)
		{
			// go back to last selected file
			gtk_file_chooser_set_filename (
					GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
					((gLastDirectory+PATH_SLASH)+gLastFileName).c_str());
		}
		else
		{
			// return focus to "matchQResults" folder wih NO selected file
			gtk_file_chooser_set_current_folder (
					GTK_FILE_CHOOSER (matchingQueueFileChooserDialog), 
					directory.c_str());
			gLastDirectory = directory;
			gLastFileName = "";
		}
		break;
	default:
		// there is no default action
		//matchingQueueFileChooserDialog = gtk_file_Chooser_new (_("Select a File"));
		break;

	}

	g_signal_connect(G_OBJECT(matchingQueueFileChooserDialog),"current-folder-changed",
			G_CALLBACK(on_mqFileChooserDirectory_changed), (void *) this); 

	g_signal_connect(G_OBJECT(matchingQueueFileChooserDialog),"selection-changed",
			G_CALLBACK(on_mqFileChooserFileSelections_changed), 
			(void *) this);

	return matchingQueueFileChooserDialog;
}

//*******************************************************************
//
void on_mqFileChooserButtonOK_clicked(MatchingQueueDialog *dialog)
{
	int i, n;
	string fileName;

	if (NULL == dialog)
		return;
	
	GSList *fileNames;
	gchar *fname;
					
	ifstream inFile;

	switch (dialog->mActionSelected) {
		case ADD_FILENAME:
			fileNames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog->mFileChooserDialog));

			n = g_slist_length(fileNames);
			for (i = 0; i < n; i++)
			{
				//g_print("Checking file: ");
				fileName = (char *)(g_slist_nth(fileNames,i)->data);
				//g_print(fileName.c_str());
				//g_print("\n");
				
				cout << "opening file " << fileName << endl;

				inFile.open(fileName.c_str());
				if (inFile.fail())
				{
					g_print("Could not open selected file!\n");
					inFile.clear();
					//g_free(g_slist_nth(fileNames,i)->data);
					//showError("Could not open selected file!");
					//break;
				}
				else if(fileName.find_last_of(".finz") != string::npos)
				{
					cout << "finz, adding to queue" << endl;
					dialog->mMatchingQueue->add(fileName);
					cout << "updating queue list" << endl;
					//dialog->updateQueueList();
					cout << "queue list updated" << endl;
				} 
				else
				{ 
					if (!isTracedFinFile(fileName))
					{
						g_print("This is not a traced dolphin fin!\n");
						//showError("This is not a traced dolphin fin!");
						//break;
					}
					else
					{
						dialog->mMatchingQueue->add(fileName);
						//dialog->updateQueueList();
					}
				}

				inFile.close();
				g_free(g_slist_nth(fileNames,i)->data);
			}
			dialog->updateQueueList();
			g_slist_free(fileNames);
			break;
		case SAVE_QUEUE:
			fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog->mFileChooserDialog));

			if (NULL == fname)
				break;

			fileName = (char *)fname;
			g_free(fname);

			//***1.4 - enforce ".fin" extension
			if (fileName.rfind(".que") != (fileName.length() - 4))
				fileName += ".que";


			g_print("Saving File: ");
			g_print(fileName.c_str());
			g_print("\n");

			try {
				dialog->mMatchingQueue->save(fileName);
			} catch (Error e) {
				showError(e.errorString());
			}
			break;
		case LOAD_QUEUE:
			fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog->mFileChooserDialog));

			if (NULL == fname)
				break;

			fileName = (char *)fname;
			g_free(fname);

			g_print("Loading File: ");
			g_print(fileName.c_str());
			g_print("\n");

			try {
				dialog->mMatchingQueue->load(fileName);
				dialog->updateQueueList();
			} catch (Error e) {
				showError(e.errorString());
			}
			break;
		case VIEW_RESULTS: //***1.1
			fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog->mFileChooserDialog));

			if (NULL == fname)
				break;

			fileName = (char *)fname;
			g_free(fname);

			g_print("Viewing File:\n  ");
			g_print(fileName.c_str());
			g_print("\n");

			try {

				MatchResults *mRes = new MatchResults();

				DatabaseFin<ColorImage> *unkFin = mRes->load(dialog->mFinDatabase, fileName);
				if (NULL == unkFin)
					break; // failure of read, not a valid result file
					
				//***1.8 - two ways to retrieve original and modifed images now
				if (string::npos == unkFin->mImageFilename.rfind("_wDarwinMods.png"))
				{
					// fin file saved by version 1.75 and earlier
					// mImageFilename retrieved from the fin file is the original
					// image filename.  If there is a modified image it is a PPM
					// file with a name based on the fin filename
					string modImgFilename = 
						unkFin->mFinFilename.substr(0,unkFin->mFinFilename.rfind(".fin"));
					ifstream fp;
					modImgFilename += "_wDarwinMods.ppm";
					fp.open(modImgFilename.c_str()); // try opending PPM file
					if (!fp.fail())
					{
						// found modified fin in version 1.75 or earlier file format
						fp.close();
						unkFin->mModifiedFinImage = new ColorImage(modImgFilename);
					}
					// original image filename was in fin file
					unkFin->mFinImage = new ColorImage(unkFin->mImageFilename);
				}
				else
				{
					// fin file saved by version 1.8 and later
					// mImageFilename retireved from the fin file is the modified
					// image filename.  The original image filename is stored
					// as a comment in the PNG image file
					ifstream fp;
					fp.open(unkFin->mImageFilename.c_str()); // try opening PNG file
					if (!fp.fail())
					{
						// found modified fin in new file format
						fp.close();
						
						//***2.0 - already loaded if *.finz
						if (NULL == unkFin->mModifiedFinImage)
							unkFin->mModifiedFinImage = new ColorImage(unkFin->mImageFilename);
						
						// name of original image extracted from modified image is without path
						// we ASSUME it is in the tracedFins folder & want to set the
						// original image filename to the path+filename
						//***1.85 - everything is now relative to the current survey area
						
						// ***2.0 - *.finz already gives us an absolute path
						if(isBasename(unkFin->mOriginalImageFilename))
						{
							// this is an OLD *.fin file with a *.pnm modified image file
							//string path = gOptions->mCurrentSurveyArea;
							//path += PATH_SLASH;
							//path += "tracedFins";
							//path += PATH_SLASH;
							// ***2.0 - the original image MUST actually be in the same folder
							//          as the *.fin file and the modified image (JHS)
							string path = unkFin->mImageFilename;
							path = path.substr(0,path.rfind(PATH_SLASH)+1); // included slash
							unkFin->mOriginalImageFilename = 
										path + unkFin->mModifiedFinImage->mOriginalImageFilename;
						}

						//***2.0 - already loaded if *.finz
						if (("" != unkFin->mOriginalImageFilename) &&
							(NULL == unkFin->mFinImage))
							unkFin->mFinImage = new ColorImage(unkFin->mOriginalImageFilename);
						
						// otherwise we leave it NULL for now
						//***2.0 - already loaded if *.finz
						if (unkFin->mImageMods.empty())
							unkFin->mImageMods = unkFin->mModifiedFinImage->mImageMods;
					}
				}

				gtk_widget_hide(dialog->mDialog); // ***2.2 - to hide this behind match results window

				MatchResultsWindow *resultsWindow = new MatchResultsWindow(
	                    unkFin,
	                    mRes, // just a pointer
	                    dialog->mFinDatabase,
	                    dialog->mMainWin,
						NULL,  // revise MatchResultsWindow so we can return here?
						//dialog->mDialog,    //***1.3
						dialog, //***1.3
						fileName,
	                    dialog->mOptions);
				resultsWindow->show();

				// do NOT delete dialog here, let it persist so we can launch 
				// multiple match results dialogs and/or return to this dialog
				// when done with viewing

				delete unkFin;

				delete mRes;

			} catch (Error e) {
				showError(e.errorString());
			}

			break;
	}
}

//*******************************************************************
//
void on_mqFileChooserButtonCancel_clicked(MatchingQueueDialog *dialog)
{

}



//*******************************************************************
//
// void on_mqFileChooserFileSelections_changed(...)
//
void on_mqFileChooserFileSelections_changed(
	GtkWidget *widget,
	gpointer userData
	)
{
	//g_print("IN on_fileChooserList_changed()\n");

	MatchingQueueDialog *dlg = (MatchingQueueDialog *) userData;

	if (NULL == dlg)
		return;
		
	gchar *fileName = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg->mFileChooserDialog));

	if (NULL == fileName)
	{
		// change in folder or something eles that "unselected" all files
		gLastFileName = "";
	}
	else
	{
		// strip path and set global last filename so we can return there next time
		gLastFileName = fileName;
		gLastFileName = gLastFileName.substr(gLastFileName.find_last_of(PATH_SLASH)+1);

		//	g_print("Last Filename : ");
		//	g_print(gLastFileName.c_str());
		//	g_print("\n");

		g_free(fileName);
	}
}


//*******************************************************************
//
// void on_mqFileChooserDirectory_changed(...)
//
void on_mqFileChooserDirectory_changed(
	GtkWidget *widget,
	gpointer userData)
{
	MatchingQueueDialog *dlg = (MatchingQueueDialog *) userData;
	
	//g_print("IN on_fileSelectionEntry_changed()\n");

	if (NULL == dlg)
		return;

	gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dlg->mFileChooserDialog));

	// set global last directory and blank filename so we can return there next time
	gLastDirectory = folder;
	gLastFileName = "";

	g_print("Last Folder : ");
	g_print(gLastDirectory.c_str());
	g_print("\n");

	g_free(folder);
}

void mqFileChooser_run_and_respond(MatchingQueueDialog *dialog)
{
	gint response = gtk_dialog_run (GTK_DIALOG (dialog->mFileChooserDialog));

	switch (response)
	{
	case GTK_RESPONSE_CANCEL :
		//g_print("Cancel HIT\n");
		// no action required ????
		break;
	case GTK_RESPONSE_ACCEPT :
		on_mqFileChooserButtonOK_clicked(dialog);
		break;
	default :
		g_print("Nada\n");
		// no other action required ??
	}

	gtk_widget_destroy(dialog->mFileChooserDialog);
	dialog->mFileChooserDialog = NULL;
}
