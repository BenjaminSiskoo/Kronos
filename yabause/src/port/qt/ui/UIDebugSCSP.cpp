/*	Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

	This file is part of Yabause.

	Yabause is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Yabause is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Yabause; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "UIDebugSCSP.h"
#include "CommonDialogs.h"

#include <QImageWriter>
#include <QGraphicsPixmapItem>
#include <QDebug>
#include <QIODevice>
#include <QTimer>
#include <QScrollBar>

// Adresse maximale acceptee par un watch : registres de slot (0x000-0x3FF)
// et registres de controle communs (0x400-0x43F). Les zones DSP (COEF 0x700,
// MADRS 0x780, MPRO 0x800) sont volontairement exclues, elles sont dumpees en
// bloc par la fenetre "Debug SCSP DSP". Doit rester coherent avec
// SCSP_REGISTER_WATCH_MAX_ADDR (scsp.h).
static const u32 kWatchMaxAddr = SCSP_REGISTER_WATCH_MAX_ADDR;

UIDebugSCSP::UIDebugSCSP( QWidget* p )
	: QDialog( p )
{
	// setup dialog
	setupUi( this );

   sbSlotNumber->setMinimum(0);
   sbSlotNumber->setMaximum(31);
   sbSlotNumber->setValue(31);
   sbSlotNumber->setValue(0);

   refreshCommonRegisters();

   refreshWatchList();

   autoRefreshTimer = new QTimer(this);
   connect(autoRefreshTimer, &QTimer::timeout, this, &UIDebugSCSP::autoRefreshTick);

#ifdef HAVE_QT_MULTIMEDIA
	audioBufferTimer = new QTimer(this);
	audioDeviceInfo = QAudioDeviceInfo::defaultOutputDevice();
	audioOutput = 0;
	slot_workbuf = 0;
	slot_buf = 0;
	slot_buf_samples = 0;
	initAudio();
#else
	// Sans Qt Multimedia, "Play Slot" ne peut rien faire : le bouton existait
	// quand meme dans le .ui et restait cliquable sans le moindre effet (aucun
	// slot on_pbPlaySlot_clicked() n'est compile). On le masque plutot que de
	// laisser une commande morte dans l'interface.
	pbPlaySlot->setVisible( false );
#endif

   // Disable DSP Register display
   gbDSPControlRegisters->setVisible( false );

	// retranslate widgets
	QtYabause::retranslateWidget( this );
}

UIDebugSCSP::~UIDebugSCSP()
{
	if (autoRefreshTimer)
		autoRefreshTimer->stop();
#ifdef HAVE_QT_MULTIMEDIA
	// L'ordre compte : on arrete le timer de remplissage et la sortie audio
	// avant de liberer les buffers, sinon audioBufferRefill() peut encore
	// etre appele sur des pointeurs liberes.
	if (audioBufferTimer)
		audioBufferTimer->stop();
	if (audioOutput)
		audioOutput->stop();
	freeAudioBuffers();
#endif
}

#ifdef HAVE_QT_MULTIMEDIA
// BUG CORRIGE : les deux buffers sont alloues avec new[] dans
// allocAudioBuffers()/stateChanged() mais etaient liberes avec "delete"
// (destructeur et stateChanged) -- comportement indefini, et en pratique une
// fuite ou un crash selon l'allocateur.
void UIDebugSCSP::freeAudioBuffers()
{
	delete[] slot_workbuf;
	delete[] slot_buf;
	slot_workbuf = 0;
	slot_buf = 0;
	slot_buf_samples = 0;
}

void UIDebugSCSP::initAudio()
{
	connect(audioBufferTimer, SIGNAL(timeout()), SLOT(audioBufferRefill()));

	isPlaying = true;

#if QT_VERSION < 0x040700
	audioFormat.setFrequency(44100);
	audioFormat.setChannels(2);
#else
	audioFormat.setSampleRate(44100);
	audioFormat.setChannelCount(2);
#endif
	audioFormat.setSampleSize(16);
	audioFormat.setCodec("audio/pcm");
	audioFormat.setByteOrder(QAudioFormat::LittleEndian);
	audioFormat.setSampleType(QAudioFormat::SignedInt);

	QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
	if (!info.isFormatSupported(audioFormat)) 
	{
		qWarning() << "Normal format not available, trying alternative";
		audioFormat = info.nearestFormat(audioFormat);
	}

	delete audioOutput;
	audioOutput = 0;
	audioOutput = new QAudioOutput(audioDeviceInfo, audioFormat, this);
	connect(audioOutput, SIGNAL(notify()), SLOT(notified()));
	connect(audioOutput, SIGNAL(stateChanged(QAudio::State)), SLOT(stateChanged(QAudio::State)));

	ScspSlotResetDebug(sbSlotNumber->value());
}

void UIDebugSCSP::notified()
{
	qWarning() << "bytesFree = " << audioOutput->bytesFree()
		<< ", " << "elapsedUSecs = " << audioOutput->elapsedUSecs()
		<< ", " << "processedUSecs = " << audioOutput->processedUSecs()
		<< ", " << "periodSize = " << audioOutput->periodSize();
}

// Nombre d'octets par trame (une trame = un echantillon par canal).
static int bytesPerFrame(const QAudioFormat &fmt)
{
#if QT_VERSION < 0x040700
	int channels = fmt.channels();
#else
	int channels = fmt.channelCount();
#endif
	int bytes = (fmt.sampleSize() / 8) * channels;
	return bytes > 0 ? bytes : 4; // 16 bits stereo par defaut
}

void UIDebugSCSP::audioBufferRefill()
{
	// Les buffers ne sont alloues qu'une fois la sortie demarree (periodSize()
	// n'a pas de valeur utile avant). Sans ce garde-fou, un tick du timer
	// arrivant avant l'allocation passait NULL a ScspSlotDebugAudio().
	if (!audioOutput || !slot_workbuf || !slot_buf)
		return;
	if (audioOutput->state() == QAudio::StoppedState)
		return;

	const int period = audioOutput->periodSize();
	if (period <= 0)
		return;

	// BUG CORRIGE : l'ancien code faisait len = (periodSize/2), alors que
	// ScspSlotDebugAudio() attend un nombre de TRAMES et remplit 2*len
	// echantillons s16 (soit 4*len octets en 16 bits stereo). Il generait donc
	// deux fois trop d'audio et n'en reinjectait que la moitie dans le
	// peripherique, ce qui donnait une lecture hachee et environ deux fois
	// trop rapide du slot.
	const int frames = period / bytesPerFrame(audioFormat);
	if (frames <= 0 || frames * 2 > slot_buf_samples)
		return;

	int chunks = audioOutput->bytesFree() / period;
	while (chunks)
	{
		if (ScspSlotDebugAudio(slot_workbuf, slot_buf, frames) == 0)
			break;
		outputDevice->write((char *)slot_buf, frames * bytesPerFrame(audioFormat));
		--chunks;
	}
}

void UIDebugSCSP::stateChanged(QAudio::State state)
{
	if (state == QAudio::IdleState)
	{
		notified();

		const int period = audioOutput ? audioOutput->periodSize() : 0;
		const int frames = period > 0 ? period / bytesPerFrame(audioFormat) : 0;
		if (frames <= 0)
			return;

		// ScspSlotDebugAudio() ecrit 2*frames u32 dans workbuf (deux canaux
		// consecutifs, bufR = workbuf + len) et 2*frames s16 dans buf.
		if (slot_buf_samples != frames * 2)
		{
			freeAudioBuffers();
			slot_workbuf = new u32[frames * 2];
			slot_buf = new s16[frames * 2];
			slot_buf_samples = frames * 2;
		}
	}
}
#endif

void UIDebugSCSP::on_sbSlotNumber_valueChanged ( int i )
{
   Q_UNUSED(i);
   refreshSlotInfo();
}

// Etait duplique en ligne dans on_sbSlotNumber_valueChanged() ; extrait
// pour etre reutilisable par le timer d'auto-refresh ci-dessous.
void UIDebugSCSP::refreshSlotInfo()
{
   char tempstr[2048];
   if (HighWram)
   {
      // On preserve la position de l'ascenseur : sans ca, l'auto-refresh
      // ramenait la vue en haut 4 fois par seconde et rendait la lecture des
      // champs du bas impossible.
      const int scroll = pteSlotInfo->verticalScrollBar()->value();
      ScspSlotDebugStats(sbSlotNumber->value(), tempstr);
      pteSlotInfo->clear();
      pteSlotInfo->appendPlainText(tempstr);
      pteSlotInfo->moveCursor(QTextCursor::Start);
      pteSlotInfo->verticalScrollBar()->setValue(scroll);
      pbSaveAsWav->setEnabled(true);
      pbSaveSlotRegisters->setEnabled(true);
   }
   else
   {
      pbSaveAsWav->setEnabled(false);
      pbSaveSlotRegisters->setEnabled(false);
   }
}

// Les registres communs n'etaient remplis qu'une seule fois, dans le
// constructeur : ils restaient donc fige sur l'etat de la puce au moment de
// l'ouverture de la fenetre. Extrait ici pour etre aussi rafraichi par
// l'auto-refresh (MVOL, SCIEB/SCIPD, les timers et DEXE bougent en permanence).
void UIDebugSCSP::refreshCommonRegisters()
{
   if (!HighWram)
      return;

   char tempstr[2048];
   const int scroll = pteCommonControlRegisters->verticalScrollBar()->value();
   ScspCommonControlRegisterDebugStats(tempstr);
   pteCommonControlRegisters->clear();
   pteCommonControlRegisters->appendPlainText(tempstr);
   pteCommonControlRegisters->moveCursor(QTextCursor::Start);
   pteCommonControlRegisters->verticalScrollBar()->setValue(scroll);
}

// Ajoute : rafraichissement automatique du slot affiche. Manquait
// completement -- il fallait auparavant changer manuellement de slot
// (meme pour revenir au meme numero) pour forcer une relecture, ce qui
// rendait impossible d'observer en direct une enveloppe/attenuation qui
// evolue pendant qu'on joue.
void UIDebugSCSP::on_cbAutoRefresh_toggled ( bool checked )
{
   if (checked)
      autoRefreshTimer->start(250); // 4x/s : assez reactif sans spammer l'UI
   else
      autoRefreshTimer->stop();
}

void UIDebugSCSP::autoRefreshTick()
{
   refreshSlotInfo();
   refreshCommonRegisters();
   updateWatchGroupTitle();
}

#ifdef HAVE_QT_MULTIMEDIA
void UIDebugSCSP::on_pbPlaySlot_clicked ()
{
	audioBufferTimer->stop();
	audioOutput->stop();

	if (isPlaying) 
	{
		ScspSlotResetDebug(sbSlotNumber->value());
		pbPlaySlot->setText(QtYabause::translate("Stop Slot"));
		outputDevice = audioOutput->start();
		if (!outputDevice)
		{
			pbPlaySlot->setText(QtYabause::translate("Play Slot"));
			CommonDialogs::error( QtYabause::translate( "Could not open the audio output device." ) );
			return;
		}

		// periodSize() n'est significatif qu'une fois la sortie demarree :
		// c'est ici qu'on dimensionne les buffers, au lieu d'attendre un
		// eventuel passage par IdleState comme le faisait l'ancien code (d'ou
		// les premiers ticks du timer avec des pointeurs nuls).
		stateChanged(QAudio::IdleState);

		isPlaying = false;
		audioBufferTimer->start(20);
	} 
	else 
	{
		pbPlaySlot->setText(QtYabause::translate("Play Slot"));
		isPlaying = true;
	}
}
#endif

void UIDebugSCSP::on_pbSaveAsWav_clicked ()
{
	// request a file to save to to user
   QString text;
   
   text = QString::asprintf("channel%02d.wav", sbSlotNumber->value());
	const QString s = CommonDialogs::getSaveFileName(text, QtYabause::translate( "Choose a location for your wav file" ), QtYabause::translate( "WAV Files (*.wav)" ) );
	
	// write image if ok
	if ( !s.isEmpty() )
		if (ScspSlotDebugAudioSaveWav(sbSlotNumber->value(), s.toLatin1()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );                  
}

void UIDebugSCSP::on_pbSaveSlotRegisters_clicked ()
{
	const QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your binary file" ), QtYabause::translate( "Binary Files (*.bin)" ) );
	if ( !s.isEmpty() )
      if (ScspSlotDebugSaveRegisters(sbSlotNumber->value(), s.toLatin1()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}

// Export complet : registres communs + les 32 slots + etat du DSP, en un
// seul fichier texte -- pour capturer d'un coup l'etat de la puce sans
// naviguer slot par slot (voir ScspSaveFullDebugReport, scsp.c).
void UIDebugSCSP::on_pbExportFullReport_clicked ()
{
	const QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your report" ), QtYabause::translate( "Text Files (*.txt)" ) );
	if ( !s.isEmpty() )
		if (ScspSaveFullDebugReport(s.toLatin1()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}

// Dump binaire brut des 512Ko de RAM son SCSP (voir ScspSaveSoundRam, scsp.c).
void UIDebugSCSP::on_pbExportSoundRam_clicked ()
{
	const QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your binary file" ), QtYabause::translate( "Binary Files (*.bin)" ) );
	if ( !s.isEmpty() )
		if (ScspSaveSoundRam(s.toLatin1()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}

// Etat du bloc CD (voir Cs2SaveDebugReport, cs2.c) -- pas de fenetre de
// debug dediee au bloc CD pour l'instant, place ici car deja le point
// central du debug audio et directement utile quand la musique CD-DA
// specifiquement reste muette alors que le mixage SCSP est correct.
void UIDebugSCSP::on_pbExportCs2Report_clicked ()
{
	const QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your report" ), QtYabause::translate( "Text Files (*.txt)" ) );
	if ( !s.isEmpty() )
		if (Cs2SaveDebugReport(s.toLatin1()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}

void UIDebugSCSP::refreshWatchList()
{
	lwWatchedAddresses->clear();
	int n = ScspGetRegisterWatchCount();
	for (int i = 0; i < n; i++)
	{
		u32 addr = ScspGetRegisterWatchAddr(i);
		if (addr == 0xFFFFFFFF)
			continue;

		// L'adresse seule ("0x216") ne dit rien : on affiche le nom du registre
		// decode par le core (ScspGetRegisterName), qui identifie aussi les
		// slots 16/17 comme les canaux CD-DA gauche/droit.
		char name[128];
		ScspGetRegisterName(addr, name, sizeof(name));

		QListWidgetItem *item = new QListWidgetItem(
			QString("0x%1  %2")
				.arg(QString("%1").arg(addr, 3, 16, QChar('0')).toUpper())
				.arg(QString::fromLatin1(name)));
		// L'adresse est stockee a part : la relire en re-parsant le libelle
		// (comme le faisait l'ancien code avec mid(2)) casse des qu'on ajoute
		// quoi que ce soit apres l'adresse.
		item->setData(Qt::UserRole, addr);
		lwWatchedAddresses->addItem(item);
	}

	updateWatchGroupTitle();
}

// Le nombre d'entrees deja enregistrees n'etait visible nulle part : on ne
// savait pas si un watch avait declenche avant d'exporter le log.
void UIDebugSCSP::updateWatchGroupTitle()
{
	const int logged = ScspGetRegisterWatchLogCount();
	gbRegisterWatch->setTitle(QString("%1 (%2)")
		.arg(QtYabause::translate("Register Watch"))
		.arg(QtYabause::translate("%1 write(s) logged").arg(logged)));
}

// Watch generique de registres : voir ScspAddRegisterWatch/ScspSaveRegisterWatchLog
// (scsp.c). Remplace l'ancien watch code en dur limite a EFSDL des slots
// 16/17 -- utilisable sur n'importe quel registre SCSP (slot ou commun).
void UIDebugSCSP::on_pbWatchAdd_clicked ()
{
	bool ok = false;
	QString text = leWatchAddress->text().trimmed();
	if (text.startsWith("0x", Qt::CaseInsensitive))
		text = text.mid(2);
	u32 addr = text.toUInt(&ok, 16);

	if (!ok || addr > kWatchMaxAddr)
	{
		CommonDialogs::information( QtYabause::translate(
			"Enter a SCSP register offset in hexadecimal, between 000 and 43F.\n"
			"Slot registers are at slot*0x20 (216 is slot 16's EFSDL/EFPAN, the "
			"CD-DA left channel), common control registers start at 400 (400 is "
			"MEM4MB/DAC18B/VER/MVOL)." ) );
		return;
	}

	if (ScspAddRegisterWatch(addr) != 0)
		CommonDialogs::information( QtYabause::translate( "Could not add this watch (already watched, or the maximum of 8 simultaneous watches was reached)." ) );
	refreshWatchList();
}

void UIDebugSCSP::on_pbWatchDel_clicked ()
{
	QListWidgetItem *item = lwWatchedAddresses->currentItem();
	if (!item)
		return;

	bool ok = false;
	u32 addr = item->data(Qt::UserRole).toUInt(&ok);
	if (ok)
		ScspDelRegisterWatch(addr);
	refreshWatchList();
}

void UIDebugSCSP::on_pbWatchExportLog_clicked ()
{
	// Exporter un log vide produisait un fichier sans aucune ligne, sans que
	// rien n'indique que c'etait normal (aucun watch n'avait declenche).
	if (ScspGetRegisterWatchLogCount() == 0)
	{
		CommonDialogs::information( QtYabause::translate( "No register write has been logged yet." ) );
		return;
	}

	const QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your log file" ), QtYabause::translate( "Text Files (*.txt)" ) );
	if ( !s.isEmpty() )
		if (ScspSaveRegisterWatchLog(s.toLatin1()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}

void UIDebugSCSP::on_pbWatchClearLog_clicked ()
{
	ScspClearRegisterWatchLog();
	updateWatchGroupTitle();
}
