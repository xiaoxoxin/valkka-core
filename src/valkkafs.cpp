/*
 * valkkafs.cpp : A simple block file system for streaming media
 * 
 * Copyright 2017, 2018 Valkka Security Ltd. and Sampsa Riikonen.
 * 
 * Authors: Sampsa Riikonen <sampsa.riikonen@iki.fi>
 * 
 * This file is part of the Valkka library.
 * 
 * Valkka is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>
 *
 */

/** 
 *  @file    valkkafs.cpp
 *  @author  Sampsa Riikonen
 *  @date    2017
 *  @version 0.10.0 
 *  
 *  @brief   A simple block file system for streaming media
 */ 

#include "valkkafs.h"


ValkkaFS::ValkkaFS(const char *device_file, const char *block_file, std::size_t blocksize, std::size_t n_blocks, bool init) : device_file(device_file), block_file(block_file), blocksize(blocksize), n_blocks(n_blocks), col_0(0), col_1(0), current_row(0), prev_row(0), pyfunc(NULL), os(block_file, std::fstream::binary | std::fstream::out | std::fstream::in), init(init)
{
    if (!os.is_open()) { // so the file did not exist
        // create
        os.open(block_file, std::fstream::out | std::fstream::binary | std::fstream::trunc); 

        // close
        if (os.is_open())
            os.close();

        // re-open
        os.open(block_file, std::fstream::binary | std::fstream::out | std::fstream::in);
    }
    
    
    std::size_t blocksize_ = std::max(std::size_t(512), blocksize-(blocksize%std::size_t(512))); // blocksize must be multiple of 512
    if (blocksize_ != blocksize) {
        valkkafslogger.log(LogLevel::normal) << "ValkkaFS: WARNING: adjusting blocksize from " << blocksize << " to " << blocksize_ << std::endl;
        blocksize = blocksize_;
    }
    
    
    os.seekp(std::streampos(0));
    tab.resize(n_cols*n_blocks, 0);
    device_size=blocksize*n_blocks;
    
    if (init) {
        clearTable();
    }
    else {
        readTable();
    }
}
    
ValkkaFS::~ValkkaFS() {
    // delete py_array;
    // Py_DECREF(arr);
    os.close();
}

void ValkkaFS::dumpTable_() {
    valkkafslogger.log(LogLevel::crazy) << "ValkkaFS: dumpTable_ : size =" << tab.size() << std::endl;
    os.seekp(std::streampos(0));
    os.write((const char*)(tab.data()), sizeof(long int)*tab.size());
    os.flush();
    os.seekp(std::streampos(0));
}

void ValkkaFS::dumpTable() {
    std::unique_lock<std::mutex> lk(this->mutex);
    dumpTable_();
}

void ValkkaFS::updateDumpTable_(std::size_t n_block) {
    std::size_t i = ind(n_block, 0);
    os.seekp(std::streampos(i*sizeof(long int)));
    os.write((const char*)(&tab[i]), sizeof(long int)); // write one row of blocktable into the blocktable file
    os.write((const char*)(&tab[i+1]), sizeof(long int));
    os.flush();
}

void ValkkaFS::readTable() {
    std::unique_lock<std::mutex> lk(this->mutex);
    std::ifstream is(block_file, std::ios::binary);
    is.read((char*)(tab.data()), sizeof(long int)*tab.size());
    is.close();
}

std::size_t ValkkaFS::ind(std::size_t i, std::size_t j) {
    if (i>=n_blocks or j>=n_cols) {
        valkkafslogger.log(LogLevel::normal) << "ValkkaFS: ind: wrong index " << i << " " << j << std::endl;
        return 0;
    }
    else {
        return i*n_cols+j;
    }
}

void ValkkaFS::setVal(std::size_t i, std::size_t j, long int val) {
    std::unique_lock<std::mutex> lk(this->mutex);
    tab[ind(i,j)]=val;
}


long int ValkkaFS::getVal(std::size_t i, std::size_t j) {
    std::unique_lock<std::mutex> lk(this->mutex);
    return tab[ind(i,j)];
}

std::size_t ValkkaFS::getBlockSeek(std::size_t n_block) {
    return n_block*blocksize;
}

std::size_t ValkkaFS::getCurrentBlockSeek() {
    return getBlockSeek(current_row);
}


std::size_t ValkkaFS::maxFrameSize() {
    return device_size / 10;
}


void ValkkaFS::reportTable(std::size_t from, std::size_t to, bool show_all) {
    std::unique_lock<std::mutex> lk(this->mutex);
    from=std::min(from, n_blocks-1);
    to  =std::min(to, n_blocks-1);
    if (to<from) {
        return;
    }
    if (to==0) {
        to=n_blocks-1;
    }
    std::size_t count;
    for(count=from;count<=to;count++) {
        if (tab[ind(count, 0)] > 0 and tab[ind(count, 1)]  > 0) {
            std::cout << count << " : " << tab[ind(count, 0)] << " " << tab[ind(count, 1)] << std::endl;
        }
        else if (show_all) {
            std::cout << count << " : - " << std::endl;
        }
    }
}



void ValkkaFS::writeBlock(bool pycall) {
    std::unique_lock<std::mutex> lk(this->mutex);
    std::string msg("");
    
    // valkkafslogger.log(LogLevel::normal) << "current_row = " << current_row << std::endl;
    // valkkafslogger.log(LogLevel::normal) << "prev_row = " << prev_row << std::endl;
    
    if (col_1==0) {
        valkkafslogger.log(LogLevel::fatal) << "ValkkaFS : writeBlock : no frames in the block.  Congrats, your ValkkaFS is broken" << std::endl;
        col_1 = tab[ind(prev_row, 1)]; // copy value from previous block
        msg="frame";
    }
    if (col_0==0) {
        valkkafslogger.log(LogLevel::fatal) << "ValkkaFS : writeBlock : WARNING: no keyframe in block " << current_row << std::endl;
        col_0 = tab[ind(prev_row, 0)]; // copy value from previous block
        msg="keyframe";
    }
    
    tab[ind(current_row, 0)] = col_0; // save values to blocktable
    tab[ind(current_row, 1)] = col_1;
    
    updateDumpTable_(current_row); // save to the blocktable file as well
    
    prev_row=current_row;
    current_row++;
    if (current_row>=n_blocks) { // wrap
        current_row=0; 
    }
    
    col_0=0;
    col_1=0;
    
    // clear old values, if any
    tab[ind(current_row,0)]=0;
    tab[ind(current_row,1)]=0;
    
    if (!pycall) {
        return;
    }
    
    if (pyfunc!=NULL) {
        PyGILState_STATE gstate;
        // std::cout << "ValkkaFS: writeBlock: obtaining Python GIL" << std::endl;
        // weird shit ahead
        // if thread.cpp => Thread::closeThread => thread joining has been called the following line will hang:
        gstate = PyGILState_Ensure();
        // std::cout << "ValkkaFS: writeBlock: obtained Python GIL" << std::endl;
        if (msg.size()>0) {
            PyObject_CallFunction(pyfunc, "s", msg.c_str());
        }
        else {
            PyObject_CallFunction(pyfunc, "n", current_row);
        }
        // std::cout << "ValkkaFS: writeBlock: releasing Python GIL" << std::endl;
        PyGILState_Release(gstate);
        // std::cout << "ValkkaFS: writeBlock: released Python GIL" << std::endl;
    }
    
}


void ValkkaFS::markFrame(long int mstimestamp) {
    col_1 = std::max(col_1, mstimestamp);
    valkkafslogger.log(LogLevel::crazy) << "ValkkaFS: markFrame: col_1 =" << col_1 << std::endl;
}
    
void ValkkaFS::markKeyFrame(long int mstimestamp) {
    col_0 = std::max(col_0, mstimestamp);
    col_1 = std::max(col_1, mstimestamp);
    valkkafslogger.log(LogLevel::crazy) << "ValkkaFS: markKeyFrame: col_0 =" << col_0 << std::endl;
}


std::string ValkkaFS::getDevice() {
    return device_file;
}
    
std::size_t ValkkaFS::getDeviceSize() {
    return device_size;
}
    
void ValkkaFS::clearDevice(bool writethrough, bool verbose) {
    std::unique_lock<std::mutex> lk(this->mutex);
    std::ofstream os(device_file, std::ios::binary);
    std::size_t i, j;
    IdNumber device_id = 0;
    char block512[512]; // miniblock (aka "sector")
    memset(&block512, 0, 512);
    memcpy(&block512, &device_id, sizeof(device_id)); // write zero device id in the beginning of the miniblock
    
    std::size_t n_miniblocks=(blocksize / std::size_t(512))-1; // miniblocks per block // -1, because we write one miniblock first
    
    for(i=0; i<n_blocks; i++) {
        if (verbose) {
            std::cout << "ValkkaFS: clearDevice: block " << i << " / " << n_blocks-1 << std::endl;
        }
        os.write((const char*)&block512, 512); // write at least one miniblock
        os.flush();
        if (writethrough) {
            for(j=0; j<n_miniblocks; j++) {
                os.write((const char*)&block512, 512);
                os.flush();
            }
        }
    }
    os.close();
}


void ValkkaFS::clearTable() {
    std::unique_lock<std::mutex> lk(this->mutex);
    std::fill(tab.begin(), tab.end(), 0);
    dumpTable_();
}

// TODO: dump blocktable to disk
// TODO: update a row in blocktable on-disk


void ValkkaFS::setCurrentBlock(std::size_t n_block) {
    std::unique_lock<std::mutex> lk(this->mutex);
    current_row=n_block;
}


void ValkkaFS::setBlockCallback(PyObject* pobj) {
    std::unique_lock<std::mutex> lk(this->mutex);
    
    // pass here, say "signal.emit" or a function/method that accepts single argument
    if (PyCallable_Check(pobj)) { // https://docs.python.org/3/c-api/type.html#c.PyTypeObject
        Py_INCREF(pobj);
        pyfunc=pobj;
    }
    else {
        valkkafslogger.log(LogLevel::fatal) << "TestThread: setCallback: needs python callable" << std::endl;
        pyfunc=NULL;
    }
}

void ValkkaFS::setArrayCall(PyObject* pyobj) {
    std::unique_lock<std::mutex> lk(this->mutex);

    Py_INCREF(pyobj);
    
    PyArrayObject *pyarr = (PyArrayObject*)pyobj;
    long int *data = (long int*)pyarr->data;
    
    /*
    setVal(0,0,11);
    setVal(0,1,12);
    setVal(1,0,21);
    setVal(1,1,22);
    */
    /*
    tab[0]=11;
    tab[1]=12;
    tab[2]=21;
    tab[3]=22;
    */
    
    memcpy(data, tab.data(), tab.size()*sizeof(long int));
    
    /*
    data[0]=11;
    data[1]=12;
    data[2]=21;
    data[3]=22;
    */
    
    Py_DECREF(pyobj);
}



std::size_t ValkkaFS::get_n_blocks() {
    return n_blocks;    
}

std::size_t ValkkaFS::get_n_cols() {
    return n_cols;
}



ValkkaFSTool::ValkkaFSTool(ValkkaFS &valkkafs) : valkkafs(valkkafs), is(valkkafs.getDevice(), std::fstream::binary | std::fstream::in) {
}

ValkkaFSTool::~ValkkaFSTool() {
    is.close();
}

void ValkkaFSTool::dumpBlock(std::size_t n_block) {
    if (n_block < 0 or n_block > valkkafs.get_n_blocks()-1) {
        return;
    }
    if (valkkafs.getVal(n_block,0)==0 and valkkafs.getVal(n_block,0)==0) {
        return;
    }
    is.seekg(std::streampos(valkkafs.getBlockSeek(n_block)));
    std::cout << std::endl << "----- Block number : " << n_block << " -----" << std::endl;
    IdNumber id;
    BasicFrame f = BasicFrame();
    while(true) {
        id = f.read(is);
        if (id==0) {
            break;
        }
        else {
            bool seek = f.isSeekable();
            std::cout << "[" << id << "] " << f;
            if (seek) {
                std::cout << " * ";
            }
            std::cout << "    " << f.dumpPayload();
            std::cout << std::endl;
        }
    }
}


ValkkaFSWriterThread::ValkkaFSWriterThread(const char *name, ValkkaFS &valkkafs, FrameFifoContext fifo_ctx) : Thread(name), valkkafs(valkkafs), infifo(name,fifo_ctx), infilter(name,&infifo), infilter_block(name,&infifo), filestream(valkkafs.getDevice(), std::fstream::binary | std::fstream::out | std::fstream::in)
{
    if (!filestream.is_open()) { // so the file did not exist
        valkkafs.clearDevice();
        /*
        // create
        filestream.open(block_file, std::fstream::out | std::fstream::binary | std::fstream::trunc);
        
        // close
        if (filestream.is_open())
            filestream.close();

        */
        // re-open
        filestream.open(valkkafs.getDevice(), std::fstream::binary | std::fstream::out | std::fstream::in);
    }
    filestream.seekp(std::streampos(0));
}
    

ValkkaFSWriterThread::~ValkkaFSWriterThread() {
    filestream.close();
}

void ValkkaFSWriterThread::run() {
    bool ok;
    unsigned short subsession_index;
    Frame* f;

    long int dt=0;
    long int mstime, oldmstime;
    mstime = getCurrentMsTimestamp();
    oldmstime = mstime;
    
    loop=true;
    
    std::size_t bytecount=0; // bytecount inside a block
    std::size_t framesize;
    IdNumber    id;
                    
    while(loop) {
        f=infifo.read(Timeout::valkkafswriterthread);
        if (!f) { // TIMEOUT
            valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread: "<< this->name <<" timeout expired!" << std::endl;
        }
        else { // GOT FRAME // this must ALWAYS BE ACCOMPANIED WITH A RECYCLE CALL
            // Handle signal frames
            if (f->getFrameClass()==FrameClass::signal) { // SIGNALFRAME
                SignalFrame *signalframe = static_cast<SignalFrame*>(f);
                handleSignal(signalframe->valkkafswriter_signal_ctx);
            } // SIGNALFRAME
            else if (f->getFrameClass()==FrameClass::basic) { // BASICFRAME
                BasicFrame *bf = static_cast<BasicFrame*>(f);
                valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread : " << this->name <<" got BasicFrame " << *bf << std::endl;
                // get the id
                auto it = slot_to_id.find(bf->n_slot);
                if (it == slot_to_id.end()) { // this slot has not been registered using an unique id
                    valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread : slot " << bf->n_slot << " does not have an id " << std::endl;
                }
                else { // HAS ID
                    id = it->second;
                    framesize = bf->calcSize();
                    // test if a single frame can fit into this filesystem
                    if (framesize+sizeof(IdNumber) > valkkafs.maxFrameSize()) {
                        valkkafslogger.log(LogLevel::fatal) << "ValkkaFSWriterThread : frame " << *f <<" too big for this ValkkaFS" << std::endl;
                    }
                    else { // FILESYSTEM OK
                        // test if the current frame fits into the current block.  An extra zero IdNumber is required to mark the block end
                        if ( (bytecount+framesize+sizeof(IdNumber)) > valkkafs.getBlockSize()) {
                            
                            /*
                            // finish this block by writing IdNumber 0
                            IdNumber zero=0;
                            filestream.write((const char*)&zero, sizeof(zero));
                            // start a new block, seek to the new position
                            bytecount=0;
                            valkkafs.writeBlock(); // inform ValkkaFS that a new block has been entered
                            // seek to the new position (which might have been wrapped to 0)
                            filestream.seekp(std::streampos(valkkafs.getCurrentBlockSeek())); 
                            // write a zero IdNumber to the beginning of the new block
                            filestream.write((const char*)&zero, sizeof(zero));
                            // rewind
                            filestream.seekp(std::streampos(valkkafs.getCurrentBlockSeek()));
                            */
                            saveCurrentBlock(true);
                            bytecount=0;
                            
                        }
                        valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread : writing frame " << *f << " " << f->dumpPayload() << std::endl;
                        bf->dump(id, filestream); // performs a flush as well
                        // inform ValkkaFS about the progression of key frames
                        // progression is measured per leading stream (stream with subsession_index == 0)
                        if (bf->subsession_index==0) { // LEAD STREAM
                            if (bf->isSeekable()) {
                                valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread : run : marking keyframe" << std::endl;
                                valkkafs.markKeyFrame(bf->mstimestamp);
                            }
                            else {
                                valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread : run : marking frame" << std::endl;
                                valkkafs.markFrame(bf->mstimestamp);
                            }
                        } // LEAD STREAM
                        bytecount+=framesize;
                        valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread : run : bytecount = " << bytecount << std::endl;
                    } // FILESYSTEM OK
                } // HAS ID
            } // BASICFRAME
            else {
                valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread : " << this->name <<" accepts only BasicFrame, got: " << *f << std::endl;
            }
            infifo.recycle(f); // always recycle
        } // GOT FRAME
        
        mstime = getCurrentMsTimestamp();
        dt = mstime-oldmstime;
        // old-style ("interrupt") signal handling
        if (dt>=Timeout::valkkafswriterthread) { // time to check the signals..
            // valkkafslogger.log(LogLevel::crazy) << "ValkkaFSWriterThread: run: interrupt, dt= " << dt << std::endl;
            handleSignals();
            oldmstime=mstime;
        }
    }
        
    if (bytecount > 0) { // the thread will exit.  Let's save and close the current block (it might not be full though)
        saveCurrentBlock(false);
    }
}



void ValkkaFSWriterThread::saveCurrentBlock(bool pycall) {
    // std::cout << "ValkkaFSWriterThread: saveCurrentBlock: start" << std::endl;
    // finish this block by writing IdNumber 0
    IdNumber zero=0;
    filestream.write((const char*)&zero, sizeof(zero));
    // start a new block, seek to the new position
    valkkafs.writeBlock(pycall); // inform ValkkaFS that a new block has been entered
    // seek to the new position (which might have been wrapped to 0)
    filestream.seekp(std::streampos(valkkafs.getCurrentBlockSeek())); 
    // write a zero IdNumber to the beginning of the new block
    filestream.write((const char*)&zero, sizeof(zero));
    // rewind
    filestream.seekp(std::streampos(valkkafs.getCurrentBlockSeek()));
    // std::cout << "ValkkaFSWriterThread: saveCurrentBlock: stop" << std::endl;
}


void ValkkaFSWriterThread::preRun() {
}
    
void ValkkaFSWriterThread::postRun() {
    
    
}

void ValkkaFSWriterThread::handleSignal(ValkkaFSWriterSignalContext &signal_ctx) {
    switch (signal_ctx.signal) {
        
        case ValkkaFSWriterSignal::exit:
            loop=false;
            break;
            
        case ValkkaFSWriterSignal::set_slot_id:
            setSlotId(signal_ctx.pars.n_slot, signal_ctx.pars.id);
            break;
            
        case ValkkaFSWriterSignal::unset_slot_id:
            unSetSlotId(signal_ctx.pars.n_slot);
            break;
            
        case ValkkaFSWriterSignal::clear_slot_id:
            clearSlotId();
            break;
            
        case ValkkaFSWriterSignal::report_slot_id:
            reportSlotId();
            break;
            
        case ValkkaFSWriterSignal::seek:
            break; // TODO
            
        
    }
}

void ValkkaFSWriterThread::sendSignal(ValkkaFSWriterSignalContext signal_ctx) {
    std::unique_lock<std::mutex> lk(this->mutex);
    this->signal_fifo.push_back(signal_ctx);
}

void ValkkaFSWriterThread::handleSignals() {
    std::unique_lock<std::mutex> lk(this->mutex);
    // handle pending signals from the signals fifo
    for (auto it = signal_fifo.begin(); it != signal_fifo.end(); ++it) { // it == pointer to the actual object (struct SignalContext)
        handleSignal(*it);
    }
    signal_fifo.clear();
}

FifoFrameFilter &ValkkaFSWriterThread::getFrameFilter() {
    return infilter;
}

FifoFrameFilter &ValkkaFSWriterThread::getBlockingFrameFilter() {
    return (FifoFrameFilter&)infilter_block;
}


void ValkkaFSWriterThread::setSlotId(SlotNumber slot, IdNumber id) {
    valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread: setSlotId: " << slot << " " << id << std::endl;
    auto it=slot_to_id.find(slot);
    if (it==slot_to_id.end()) { // this slot does not exist
        slot_to_id.insert(std::make_pair(slot, id));
    }
    else {
        valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread: setSlotId: slot " << slot << " reserved" << std::endl;
    }
}
    
void ValkkaFSWriterThread::unSetSlotId(SlotNumber slot) {
    valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread: unSetSlotId: " << slot << std::endl;
    auto it=slot_to_id.find(slot);
    if (it==slot_to_id.end()) { // this slot does not exist
        valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread: unSetSlotId: no such slot " << slot << std::endl;
    }
    else {
        slot_to_id.erase(it);
    }
}
    
void ValkkaFSWriterThread::clearSlotId() {
    valkkafslogger.log(LogLevel::debug) << "ValkkaFSWriterThread: clearSlotId: " << std::endl;
    slot_to_id.clear();
}

void ValkkaFSWriterThread::reportSlotId() {
    std::cout << "ValkkaFSWriterThread: reportSlotId: " << std::endl;
    for(auto it=slot_to_id.begin(); it!=slot_to_id.end(); ++it) {
        std::cout << "ValkkaFSWriterThread: reportSlotId: " << it->first << " --> " << it->second << std::endl;
    }
}

    
void ValkkaFSWriterThread::seek(std::size_t n_block) {
    filestream.seekp(std::streampos(valkkafs.getBlockSeek(n_block)));
}


void ValkkaFSWriterThread::setSlotIdCall(SlotNumber slot, IdNumber id) {
    ValkkaFSWriterSignalContext signal_ctx;
    ValkkaFSWriterSignalPars    pars;
    
    pars.n_slot = slot;
    pars.id     = id;

    signal_ctx.signal = ValkkaFSWriterSignal::set_slot_id;
    signal_ctx.pars   = pars;
    
    // prepare a signal frame
    SignalFrame f = SignalFrame();
    f.valkkafswriter_signal_ctx = signal_ctx;
    // .. and send it to the queue
    infilter.run(&f);
}


void ValkkaFSWriterThread::unSetSlotIdCall(SlotNumber slot) {
    ValkkaFSWriterSignalContext signal_ctx;
    ValkkaFSWriterSignalPars    pars;
    
    pars.n_slot = slot;

    signal_ctx.signal = ValkkaFSWriterSignal::unset_slot_id;
    signal_ctx.pars   = pars;
    
    // prepare a signal frame
    SignalFrame f = SignalFrame();
    f.valkkafswriter_signal_ctx = signal_ctx;
    // .. and send it to the queue
    infilter.run(&f);
}

void ValkkaFSWriterThread::clearSlotIdCall() {
    ValkkaFSWriterSignalContext signal_ctx;
    ValkkaFSWriterSignalPars    pars;
    
    signal_ctx.signal = ValkkaFSWriterSignal::clear_slot_id;
    signal_ctx.pars   = pars;
    
    // prepare a signal frame
    SignalFrame f = SignalFrame();
    f.valkkafswriter_signal_ctx = signal_ctx;
    // .. and send it to the queue
    infilter.run(&f);
}

void ValkkaFSWriterThread::reportSlotIdCall() {
    ValkkaFSWriterSignalContext signal_ctx;
    signal_ctx.signal = ValkkaFSWriterSignal::report_slot_id;
    
    // prepare a signal frame
    SignalFrame f = SignalFrame();
    f.valkkafswriter_signal_ctx = signal_ctx;
    // .. and send it to the queue
    infilter.run(&f);
}

void ValkkaFSWriterThread::seekCall(std::size_t n_block) {
    ValkkaFSWriterSignalContext signal_ctx;
    ValkkaFSWriterSignalPars    pars;
    
    pars.n_block= n_block;

    signal_ctx.signal = ValkkaFSWriterSignal::seek;
    signal_ctx.pars   = pars;
    
    // prepare a signal frame
    SignalFrame f = SignalFrame();
    f.valkkafswriter_signal_ctx = signal_ctx;
    // .. and send it to the queue
    infilter.run(&f);
}


void ValkkaFSWriterThread::requestStopCall() {
    if (!this->has_thread) { return; } // thread never started
    if (stop_requested) { return; }    // can be requested only once
    stop_requested = true;

    // use the old-style "interrupt" way of sending signals
    ValkkaFSWriterSignalContext signal_ctx;
    signal_ctx.signal = ValkkaFSWriterSignal::exit;
    
    this->sendSignal(signal_ctx);
}


