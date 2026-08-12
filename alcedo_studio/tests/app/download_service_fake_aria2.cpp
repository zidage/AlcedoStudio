//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#include <QCoreApplication>
#include <QThread>

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QThread::sleep(3);
  return 0;
}
