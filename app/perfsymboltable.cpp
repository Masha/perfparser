/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/
#include "perfsymboltable.h"
#include "perfunwind.h"
#include <dwarf.h>

#include <QDir>
#include <QDebug>
#include <QStack>

#include <cstring>
#include <cxxabi.h>

PerfSymbolTable::PerfSymbolTable(quint32 pid, Dwfl_Callbacks *callbacks, PerfUnwind *parent) :
    m_perfMapFile(QString::fromLatin1("/tmp/perf-%1.map").arg(pid)),
    m_unwind(parent), m_lastMmapAddedTime(0),
    m_nextMmapOverwrittenTime(std::numeric_limits<quint64>::max()), m_callbacks(callbacks)
{
    m_dwfl = dwfl_begin(m_callbacks);
}

PerfSymbolTable::~PerfSymbolTable()
{
    dwfl_end(m_dwfl);
}

static pid_t nextThread(Dwfl *dwfl, void *arg, void **threadArg)
{
    /* Stop after first thread. */
    if (*threadArg != 0)
        return 0;

    *threadArg = arg;
    return dwfl_pid(dwfl);
}

static bool accessDsoMem(Dwfl *dwfl, const PerfUnwind::UnwindInfo *ui, Dwarf_Addr addr,
                         Dwarf_Word *result)
{
    // TODO: Take the pgoff into account? Or does elf_getdata do that already?
    Dwfl_Module *mod = dwfl_addrmodule(dwfl, addr);
    if (!mod) {
        mod = ui->unwind->reportElf(addr, ui->sample->pid(), ui->sample->time());
        if (!mod)
            return false;
    }

    Dwarf_Addr bias;
    Elf_Scn *section = dwfl_module_address_section(mod, &addr, &bias);

    if (section) {
        Elf_Data *data = elf_getdata(section, NULL);
        if (data && data->d_buf && data->d_size > addr) {
            std::memcpy(result, static_cast<char *>(data->d_buf) + addr, sizeof(Dwarf_Word));
            return true;
        }
    }

    return false;
}

static bool memoryRead(Dwfl *dwfl, Dwarf_Addr addr, Dwarf_Word *result, void *arg)
{
    PerfUnwind::UnwindInfo *ui = static_cast<PerfUnwind::UnwindInfo *>(arg);

    /* Check overflow. */
    if (addr + sizeof(Dwarf_Word) < addr) {
        qWarning() << "Invalid memory read requested by dwfl" << addr;
        ui->firstGuessedFrame = ui->frames.length();
        return false;
    }

    const QByteArray &stack = ui->sample->userStack();

    quint64 start = ui->sample->registerValue(
                PerfRegisterInfo::s_perfSp[ui->unwind->architecture()]);
    quint64 end = start + stack.size();

    if (addr < start || addr + sizeof(Dwarf_Word) > end) {
        // not stack, try reading from ELF
        if (ui->unwind->ipIsInKernelSpace(addr))
            dwfl = ui->unwind->dwfl(PerfUnwind::s_kernelPid, ui->sample->time());
        if (!accessDsoMem(dwfl, ui, addr, result)) {
            ui->firstGuessedFrame = ui->frames.length();
            const QHash<quint64, Dwarf_Word> &stackValues = ui->stackValues[ui->sample->pid()];
            auto it = stackValues.find(addr);
            if (it == stackValues.end()) {
                return false;
            } else {
                *result = *it;
            }
        }
    } else {
        std::memcpy(result, &(stack.data()[addr - start]), sizeof(Dwarf_Word));
        ui->stackValues[ui->sample->pid()][addr] = *result;
    }
    return true;
}

static bool setInitialRegisters(Dwfl_Thread *thread, void *arg)
{
    const PerfUnwind::UnwindInfo *ui = static_cast<PerfUnwind::UnwindInfo *>(arg);
    quint64 abi = ui->sample->registerAbi() - 1; // ABI 0 means "no registers"
    Q_ASSERT(abi < PerfRegisterInfo::s_numAbis);
    uint architecture = ui->unwind->architecture();
    uint numRegs = PerfRegisterInfo::s_numRegisters[architecture][abi];
    Dwarf_Word dwarfRegs[numRegs];
    for (uint i = 0; i < numRegs; ++i)
        dwarfRegs[i] = ui->sample->registerValue(
                    PerfRegisterInfo::s_perfToDwarf[architecture][abi][i]);

    // Go one frame up to get the rest of the stack at interworking veneers.
    if (ui->isInterworking) {
        dwarfRegs[PerfRegisterInfo::s_dwarfIp[architecture][abi]] =
                dwarfRegs[PerfRegisterInfo::s_dwarfLr[architecture][abi]];
    }

    uint dummyBegin = PerfRegisterInfo::s_dummyRegisters[architecture][0];
    uint dummyNum = PerfRegisterInfo::s_dummyRegisters[architecture][1] - dummyBegin;

    if (dummyNum > 0) {
        Dwarf_Word dummyRegs[dummyNum];
        std::memset(dummyRegs, 0, dummyNum * sizeof(Dwarf_Word));
        if (!dwfl_thread_state_registers(thread, dummyBegin, dummyNum, dummyRegs))
            return false;
    }

    return dwfl_thread_state_registers(thread, 0, numRegs, dwarfRegs);
}

static const Dwfl_Thread_Callbacks threadCallbacks = {
    nextThread, NULL, memoryRead, setInitialRegisters, NULL, NULL
};

static bool findInExtraPath(QFileInfo &path, const QString &fileName)
{
    path.setFile(path.absoluteFilePath() + QDir::separator() + fileName);
    if (path.isFile())
        return true;
    else if (!path.isDir())
        return false;

    QDir absDir = path.absoluteDir();
    foreach (const QString &entry, absDir.entryList(QStringList(),
                                                    QDir::Dirs | QDir::NoDotAndDotDot)) {
        path.setFile(absDir, entry);
        if (findInExtraPath(path, fileName))
            return true;
    }
    return false;
}

void PerfSymbolTable::registerElf(const PerfRecordMmap &mmap, const QString &appPath,
                                  const QString &systemRoot, const QString &extraLibsPath)
{
    bool cacheInvalid = false;
    quint64 overwritten = std::numeric_limits<quint64>::max();
    for (auto i = m_elfs.begin(); i != m_elfs.end() && i.key() < mmap.addr() + mmap.len(); ++i) {
        if (i.key() + i->length <= mmap.addr())
            continue;

        if (mmap.time() > i->timeAdded)
            i->timeOverwritten = qMin(i->timeOverwritten, mmap.time());
        else
            overwritten = qMin(i->timeAdded, overwritten);

        // Overlapping module. Clear the cache
        cacheInvalid = true;
    }

    // There is no need to clear the symbol or location caches in PerfUnwind. Some locations become
    // stale this way, but we still need to keep their IDs, as the receiver might still use them for
    // past frames.
    if (cacheInvalid)
        clearCache();

    QLatin1String filePath(mmap.filename());
    QFileInfo fileInfo(filePath);
    QFileInfo fullPath;
    if (mmap.pid() != PerfUnwind::s_kernelPid) {
        fullPath.setFile(appPath);
        if (!findInExtraPath(fullPath, fileInfo.fileName())) {
            bool found = false;
            foreach (const QString &extraPath, extraLibsPath.split(QLatin1Char(':'))) {
                fullPath.setFile(extraPath);
                if (findInExtraPath(fullPath, fileInfo.fileName())) {
                    found = true;
                    break;
                }
            }
            if (!found)
                fullPath.setFile(systemRoot + filePath);
        }
    } else { // kernel
        fullPath.setFile(systemRoot + filePath);
    }

    m_elfs.insertMulti(mmap.addr(), ElfInfo(fullPath, mmap.len(), mmap.time(), overwritten,
                                            fullPath.isFile()));
}

static QByteArray dieName(Dwarf_Die *die)
{
    Dwarf_Attribute attr;
    Dwarf_Attribute *result = dwarf_attr_integrate(die, DW_AT_MIPS_linkage_name, &attr);
    if (!result)
        result = dwarf_attr_integrate(die, DW_AT_linkage_name, &attr);

    const char *name = dwarf_formstring(result);
    if (!name)
        name = dwarf_diename(die);

    return name ? name : "";
}

static QByteArray demangle(const QByteArray &mangledName)
{
    if (mangledName.length() < 3) {
        return mangledName;
    } else {
        static size_t demangleBufferLength = 0;
        static char *demangleBuffer = nullptr;

        // Require GNU v3 ABI by the "_Z" prefix.
        if (mangledName[0] == '_' && mangledName[1] == 'Z') {
            int status = -1;
            char *dsymname = abi::__cxa_demangle(mangledName, demangleBuffer, &demangleBufferLength,
                                                 &status);
            if (status == 0)
                return demangleBuffer = dsymname;
        }
    }
    return mangledName;
}

int PerfSymbolTable::insertSubprogram(Dwarf_Die *top, Dwarf_Addr entry, const QByteArray &binary,
                                      qint32 inlineCallLocationId, bool isKernel)
{
    int line = 0;
    dwarf_decl_line(top, &line);
    int column = 0;
    dwarf_decl_column(top, &column);
    const QByteArray file = dwarf_decl_file(top);

    int locationId = m_unwind->resolveLocation(PerfUnwind::Location(entry, file, line, column,
                                                                    inlineCallLocationId));
    m_unwind->resolveSymbol(locationId, PerfUnwind::Symbol(demangle(dieName(top)), binary,
                                                           isKernel));

    return locationId;
}

int PerfSymbolTable::parseDie(Dwarf_Die *top, const QByteArray &binary, Dwarf_Files *files,
                              Dwarf_Addr entry, bool isKernel, const QStack<DieAndLocation> &stack)
{
    int tag = dwarf_tag(top);
    switch (tag) {
    case DW_TAG_inlined_subroutine: {
        PerfUnwind::Location location(entry);
        Dwarf_Attribute attr;
        Dwarf_Word val = 0;
        location.file
                = (dwarf_formudata(dwarf_attr(top, DW_AT_call_file, &attr), &val) == 0)
                ? dwarf_filesrc (files, val, NULL, NULL) : "";
        location.line
                = (dwarf_formudata(dwarf_attr(top, DW_AT_call_line, &attr), &val) == 0)
                ? val : -1;
        location.column
                = (dwarf_formudata(dwarf_attr(top, DW_AT_call_column, &attr), &val) == 0)
                ? val : -1;

        auto it = stack.end();
        --it;
        while (it != stack.begin()) {
            location.parentLocationId = (--it)->locationId;
            if (location.parentLocationId != -1)
                break;
        }

        int callLocationId = m_unwind->resolveLocation(location);
        return insertSubprogram(top, entry, binary, callLocationId, isKernel);
    }
    case DW_TAG_subprogram:
        return insertSubprogram(top, entry, binary, -1, isKernel);
    default:
        return -1;
    }
}

void PerfSymbolTable::parseDwarf(Dwarf_Die *cudie, Dwarf_Addr bias, const QByteArray &binary,
                                 bool isKernel)
{
    // Iterate through all dwarf sections and establish parent/child relations for inline
    // subroutines. Add all symbols to m_symbols and special frames for start points of inline
    // instances to m_addresses.

    QStack<DieAndLocation> stack;
    stack.push_back({*cudie, -1});

    Dwarf_Files *files = 0;
    dwarf_getsrcfiles(cudie, &files, NULL);

    while (!stack.isEmpty()) {
        Dwarf_Die *top = &(stack.last().die);
        Dwarf_Addr entry = 0;
        if (dwarf_entrypc(top, &entry) == 0 && entry != 0)
            stack.last().locationId = parseDie(top, binary, files, entry + bias, isKernel, stack);

        Dwarf_Die child;
        if (dwarf_child(top, &child) == 0) {
            stack.push_back({child, -1});
        } else {
            do {
                Dwarf_Die sibling;
                // Mind that stack.last() can change during this loop. So don't use "top" below.
                const bool hasSibling = (dwarf_siblingof(&(stack.last().die), &sibling) == 0);
                stack.pop_back();
                if (hasSibling) {
                    stack.push_back({sibling, -1});
                    break;
                }
            } while (!stack.isEmpty());
        }
    }
}

Dwfl_Module *PerfSymbolTable::reportElf(QMap<quint64, PerfSymbolTable::ElfInfo>::ConstIterator i)
{
    if (i == m_elfs.end() || !i.value().found)
        return 0;
    Dwfl_Module *ret = dwfl_report_elf(
                m_dwfl, i.value().file.fileName().toLocal8Bit().constData(),
                i.value().file.absoluteFilePath().toLocal8Bit().constData(), -1, i.key(),
                false);
    if (!ret)
        qWarning() << "failed to report" << i.value().file.absoluteFilePath() << "for"
                   << QString::fromLatin1("0x%1").arg(i.key(), 0, 16).toLocal8Bit().constData()
                   << ":" << dwfl_errmsg(dwfl_errno());

    if (m_lastMmapAddedTime < i->timeAdded)
        m_lastMmapAddedTime = i->timeAdded;
    if (m_nextMmapOverwrittenTime > i->timeOverwritten)
        m_nextMmapOverwrittenTime = i->timeOverwritten;

    return ret;
}

QMap<quint64, PerfSymbolTable::ElfInfo>::ConstIterator
PerfSymbolTable::findElf(quint64 ip, quint64 timestamp) const
{
    QMap<quint64, ElfInfo>::ConstIterator i = m_elfs.upperBound(ip);
    if (i == m_elfs.end() || i.key() != ip) {
        if (i != m_elfs.begin())
            --i;
        else
            return m_elfs.end();
    }

//    /* On ARM, symbols for thumb functions have 1 added to
//     * the symbol address as a flag - remove it */
//    if ((ehdr.e_machine == EM_ARM) &&
//        (map->type == MAP__FUNCTION) &&
//        (sym.st_value & 1))
//        --sym.st_value;
//
//    ^ We don't have to do this here as libdw is supposed to handle it from version 0.160.

    while (true) {
        if (i->timeAdded <= timestamp && i->timeOverwritten > timestamp)
            return (i.key() + i->length > ip) ? i : m_elfs.end();

        if (i == m_elfs.begin())
            return m_elfs.end();

        --i;
    }
}

int PerfSymbolTable::lookupFrame(Dwarf_Addr ip, quint64 timestamp, bool isKernel,
                                 bool *isInterworking)
{
    Q_ASSERT(timestamp >= m_lastMmapAddedTime);
    Q_ASSERT(timestamp < m_nextMmapOverwrittenTime);

    auto it = m_addressCache.constFind(ip);
    if (it != m_addressCache.constEnd()) {
        *isInterworking = it->isInterworking;
        return it->locationId;
    }

    Dwfl_Module *mod = m_dwfl ? dwfl_addrmodule(m_dwfl, ip) : 0;
    QByteArray elfFile;

    quint64 elfStart = 0;

    auto elfIt = findElf(ip, timestamp);
    if (elfIt != m_elfs.end()) {
        elfFile = elfIt.value().file.fileName().toLocal8Bit();
        elfStart = elfIt.key();
        if (m_dwfl && !mod)
            mod = reportElf(elfIt);
    }

    PerfUnwind::Location addressLocation(
                (m_unwind->architecture() != PerfRegisterInfo::ARCH_ARM || (ip & 1))
                ? ip : ip + 1);
    PerfUnwind::Location functionLocation(addressLocation);

    QByteArray symname;
    GElf_Sym sym;
    GElf_Off off = 0;

    if (mod) {
        // For addrinfo we need the raw pointer into symtab, so we need to adjust ourselves.
        symname = dwfl_module_addrinfo(mod, addressLocation.address, &off, &sym, 0, 0, 0);
        Dwfl_Line *srcLine = dwfl_module_getsrc(mod, addressLocation.address);
        if (srcLine) {
            addressLocation.file = dwfl_lineinfo(srcLine, NULL, &addressLocation.line,
                                                 &addressLocation.column, NULL, NULL);
        }

        if (off == addressLocation.address) {// no symbol found
            functionLocation.address = elfStart; // use the start of the elf as "function"
            addressLocation.parentLocationId = m_unwind->resolveLocation(functionLocation);
        } else {
            Dwarf_Addr bias = 0;
            functionLocation.address -= off; // in case we don't find anything better
            Dwarf_Die *die = dwfl_module_addrdie(mod, addressLocation.address, &bias);

            Dwarf_Die *scopes = NULL;
            int nscopes = dwarf_getscopes(die, addressLocation.address - bias, &scopes);

            for (int i = 0; i < nscopes; ++i) {
                Dwarf_Die *scope = &scopes[i];
                const int tag = dwarf_tag(scope);
                if (tag == DW_TAG_subprogram || tag == DW_TAG_inlined_subroutine) {
                    Dwarf_Addr entry = 0;
                    dwarf_entrypc(scope, &entry);
                    functionLocation.address = entry + bias;
                    functionLocation.file = dwarf_decl_file(scope);
                    dwarf_decl_line(scope, &functionLocation.line);
                    dwarf_decl_column(scope, &functionLocation.column);
                    break;
                }
            }
            free(scopes);

            addressLocation.parentLocationId = m_unwind->lookupLocation(functionLocation);
            if (die && !m_unwind->hasSymbol(addressLocation.parentLocationId))
                parseDwarf(die, bias, elfFile, isKernel);
            if (addressLocation.parentLocationId == -1)
                addressLocation.parentLocationId = m_unwind->resolveLocation(functionLocation);
        }
        if (!m_unwind->hasSymbol(addressLocation.parentLocationId)) {
            // no sufficient debug information. Use what we already know
            m_unwind->resolveSymbol(addressLocation.parentLocationId,
                                    PerfUnwind::Symbol(demangle(symname), elfFile, isKernel));
        }
    } else {
        symname = symbolFromPerfMap(addressLocation.address, &off);
        if (off)
            functionLocation.address -= off;
        else
            functionLocation.address = elfStart;
        addressLocation.parentLocationId = m_unwind->resolveLocation(functionLocation);
        if (!m_unwind->hasSymbol(addressLocation.parentLocationId)) {
            m_unwind->resolveSymbol(addressLocation.parentLocationId,
                                    PerfUnwind::Symbol(symname, elfFile, isKernel));
        }
    }

    Q_ASSERT(addressLocation.parentLocationId != -1);
    Q_ASSERT(m_unwind->hasSymbol(addressLocation.parentLocationId));

    int locationId = m_unwind->resolveLocation(addressLocation);
    *isInterworking = (symname == "$at");
    m_addressCache.insert(ip, {locationId, *isInterworking});
    return locationId;
}

static bool operator<(const PerfSymbolTable::PerfMapSymbol &a,
                      const PerfSymbolTable::PerfMapSymbol &b)
{
    return a.start < b.start;
}

QByteArray PerfSymbolTable::symbolFromPerfMap(quint64 ip, GElf_Off *offset) const
{
    QVector<PerfMapSymbol>::ConstIterator sym
            = std::upper_bound(m_perfMap.begin(), m_perfMap.end(), PerfMapSymbol(ip));
    if (sym != m_perfMap.begin()) {
        --sym;
        if (sym->start <= ip && sym->start + sym->length > ip) {
            *offset = ip - sym->start;
            return sym->name;
        }
    }

    *offset = 0;
    return QByteArray();
}

void PerfSymbolTable::updatePerfMap()
{
    if (!m_perfMapFile.isOpen())
        m_perfMapFile.open(QIODevice::ReadOnly);

    bool readLine = false;
    while (!m_perfMapFile.atEnd()) {
        QByteArrayList line = m_perfMapFile.readLine().split(' ');
        if (line.length() >= 3) {
            bool ok = false;
            quint64 start = line.takeFirst().toULongLong(&ok, 16);
            if (!ok)
                continue;
            quint64 length = line.takeFirst().toULongLong(&ok, 16);
            if (!ok)
                continue;
            QByteArray name = line.join(' ').trimmed();
            m_perfMap.append(PerfMapSymbol(start, length, name));
            readLine = true;
        }
    }

    if (readLine)
        std::sort(m_perfMap.begin(), m_perfMap.end());
}

bool PerfSymbolTable::containsAddress(quint64 address) const
{
    if (m_elfs.isEmpty())
        return false;

    const auto last = (--m_elfs.constEnd());
    const auto first = m_elfs.constBegin();
    return first.key() <= address && last.key() + last.value().length > address;
}

Dwfl *PerfSymbolTable::attachDwfl(quint32 pid, quint64 timestamp, void *arg)
{
    if (timestamp < m_lastMmapAddedTime || timestamp >= m_nextMmapOverwrittenTime)
        clearCache();
    else if (static_cast<pid_t>(pid) == dwfl_pid(m_dwfl))
        return m_dwfl; // Already attached, nothing to do

    // Report some random elf, so that dwfl guesses the target architecture.
    for (auto it = m_elfs.constBegin(), end = m_elfs.constEnd(); it != end; ++it) {
        if (!it->found || it->timeAdded > timestamp || it->timeOverwritten <= timestamp)
            continue;
        if (dwfl_report_elf(m_dwfl, it.value().file.fileName().toLocal8Bit().constData(),
                            it.value().file.absoluteFilePath().toLocal8Bit().constData(), -1,
                            it.key(), false)) {
            m_lastMmapAddedTime = it->timeAdded;
            m_nextMmapOverwrittenTime = it->timeOverwritten;
            break;
        }
    }

    if (!dwfl_attach_state(m_dwfl, 0, pid, &threadCallbacks, arg)) {
        qWarning() << pid << "failed to attach state" << dwfl_errmsg(dwfl_errno());
        return nullptr;
    }

    return m_dwfl;
}

void PerfSymbolTable::clearCache()
{
    m_lastMmapAddedTime = 0;
    m_nextMmapOverwrittenTime = std::numeric_limits<quint64>::max();
    m_addressCache.clear();
    m_perfMap.clear();
    if (m_perfMapFile.isOpen())
        m_perfMapFile.reset();

    // Throw out the dwfl state
    dwfl_end(m_dwfl);
    m_dwfl = dwfl_begin(m_callbacks);
}