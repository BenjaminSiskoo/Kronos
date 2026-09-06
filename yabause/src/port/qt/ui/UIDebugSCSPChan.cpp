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
#include "UIDebugSCSPChan.h"
#include "CommonDialogs.h"

#include <QImageWriter>
#include <QGraphicsPixmapItem>
#include <QDebug>
#include <QIODevice>
#include <QTimer>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QPainter>

UIDebugSCSPChan::UIDebugSCSPChan(QWidget* p)
   : QDialog(p)
{
   QVBoxLayout *verticalLayout = new QVBoxLayout(this);

   scsp_debug_set_mode(1);

   scsp_debug_instrument_clear();

   envelope_colors[0] = Qt::red;
   envelope_colors[1] = Qt::green;
   envelope_colors[2] = Qt::blue;
   envelope_colors[3] = Qt::cyan;

   for (int i = 0; i < 24; i++)
   {
      checkbox[i] = new QCheckBox("null");
      checkbox[i]->setChecked(false);
      verticalLayout->addWidget(checkbox[i]);
   }

   resize(720, 480);

   timer = new QTimer(this);
   timer->setInterval(50);//20 fps
   timer->start();

   connect(timer, SIGNAL(timeout()), SLOT(update_window()));

   // retranslate widgets
   QtYabause::retranslateWidget(this);

   this->show();
}

UIDebugSCSPChan::~UIDebugSCSPChan()
{
   scsp_debug_set_mode(0);

   delete timer;
   for (int i = 0; i < 24; i++)
      delete checkbox[i];
}

void UIDebugSCSPChan::paintEvent(QPaintEvent *event)
{
   Q_UNUSED(event);

   QPainter painter(this);

   painter.setRenderHint(QPainter::Antialiasing);
   
   int channel_width = 16;
   double max_height = 64;
   int spacer = 4;

   int start_x = 64;
   int start_y = 8;

   int space = 0;

   QRect rect;

   // Legende des couleurs (etait absente : impossible de savoir ce que
   // chaque couleur signifie sans lire le code source). Les libelles passent
   // maintenant par QtYabause::translate() comme le reste de l'interface : ils
   // etaient ecrits en dur, et melangeaient en plus anglais et francais.
   painter.setPen(Qt::black);
   painter.drawText(8, start_y + 10, QtYabause::translate("Slot"));
   {
      struct { QString label; QColor color; } legend[] = {
         { QtYabause::translate("Attack"),  envelope_colors[0] },
         { QtYabause::translate("Decay 1"), envelope_colors[1] },
         { QtYabause::translate("Decay 2"), envelope_colors[2] },
         { QtYabause::translate("Release"), envelope_colors[3] },
      };
      int ly = (int)(start_y + max_height + 24);
      int lx = start_x;
      for (auto &l : legend)
      {
         painter.fillRect(QRect(lx, ly, 10, 10), l.color);
         painter.setPen(Qt::black);
         painter.drawText(lx + 14, ly + 9, l.label);
         lx += 90;
      }
      painter.fillRect(QRect(lx, ly, 10, 10), Qt::darkGray);
      painter.drawText(lx + 14, ly + 9, QtYabause::translate("Inactive slot"));
   }

   for (int i = 0; i < 32; i++)
   {
      int env = 0, state = 0;
      scsp_debug_get_envelope(i, &env, &state);

      double env_ratio = (1023.0 - env)/1023.0;
      int bar_x = start_x + space + (i * channel_width);
      // La barre etait dessinee en simple contour : a 16 pixels de large la
      // couleur de phase etait a peine visible. On remplit, en gardant un
      // contour sombre pour separer deux slots voisins de meme couleur.
      rect = QRect(bar_x, 8, channel_width, (int)(max_height * env_ratio));
      painter.fillRect(rect, colorForEnvelopeState(state));
      painter.setPen(Qt::darkGray);
      painter.drawRect(rect);

      // Numero du slot sous chaque barre (etait absent : impossible de
      // savoir quelle barre correspond a quel slot sans compter a la main).
      painter.setPen(Qt::black);
      painter.save();
      QFont f = painter.font();
      f.setPointSize(6);
      painter.setFont(f);
      painter.drawText(QRect(bar_x - 2, (int)(start_y + max_height + 2), channel_width + 4, 12),
                        Qt::AlignHCenter, QString::number(i));
      painter.restore();

      space += spacer;
   }
}

// BUG CORRIGE : le code indexait directement envelope_colors[state] avec
// la valeur brute de l'enum EnvelopeStates (ATTACK=1, DECAY1=2, DECAY2=3,
// RELEASE=4), alors que envelope_colors ne contient que 4 entrees
// (indices 0-3). Consequences : RELEASE (etat tres frequent, note qui
// s'eteint) lisait envelope_colors[4] -- hors du tableau, comportement
// indefini -- et toutes les autres phases affichaient la couleur de la
// phase suivante (decalage d'un cran). Un slot jamais joue (envelope=0,
// etat par defaut apres reset) affichait en plus la meme couleur que
// ATTACK, le rendant indiscernable d'un slot reellement actif.
QColor UIDebugSCSPChan::colorForEnvelopeState(int state) const
{
   if (state < 1 || state > 4)
      return Qt::darkGray; // slot inactif / jamais joue depuis le reset
   return envelope_colors[state - 1];
}

void UIDebugSCSPChan::update_window()
{
   QString address;

   for (int i = 0; i < 24; i++)
   {
      u32 sa = 0;
      int muted = 0;
      scsp_debug_instrument_get_data(i, &sa, &muted);
      
      address = QString::asprintf("%05X", sa);

      checkbox[i]->setText(address);

      if (checkbox[i]->isChecked())
         scsp_debug_instrument_set_mute(sa, 1);//mute this instrument
      else
         scsp_debug_instrument_set_mute(sa, 0);
   }

   // repaint() force un redessin synchrone immediat a chaque tick du timer ;
   // update() laisse Qt fusionner les demandes et repeindre une seule fois par
   // cycle d'evenements, ce qui suffit largement a 20 images/s.
   update();
}
