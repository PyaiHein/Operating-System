#include "VirtualMachine.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <iostream>
#include <unistd.h>
#include "Machine.h"
#include <queue>
#include <cstring>
#include <fcntl.h>
#include <sstream>
using namespace std;
TVMStatus VMMutexCreate(TVMMutexIDRef mutexref);

int rootCount = 0;

//******************************FAT STuff***************************************
extern"C"
{
	class BPBinf
	{
		public:
		uint16_t BPB_BytsPerSec;
		uint8_t BPB_SecPerClus;
		uint16_t BPB_RsvdSecCnt;
		uint8_t BPB_NumFATs;
		uint16_t BPB_RootEntCnt;
		uint16_t BPB_TotSec16;
		uint8_t BPB_Media;
		uint16_t BPB_FATSz16;
		uint16_t BPB_SecPerTrk;
		uint16_t BPB_NumHeads;
		uint32_t BPB_HiddSec;
		uint32_t BPB_TotSec32;
		uint8_t BS_DrvNum;
		uint8_t BS_Reserved1;
		uint8_t BS_BootSig;
		uint16_t FirstRootSector;
    	uint16_t RootDirectorySectors;
    	uint16_t FirstDataSector;
    	uint16_t ClusterCount;
	};

	class DirectInf
	{
		public:
	    char DIR_Name[12];
	    uint8_t DIR_Attr;
	    uint8_t DIR_NTRes;
	    uint8_t DIR_CrtTimeTenth;
	    uint16_t DIR_CrtTime;
	    uint16_t DIR_CrtDate;
	    uint16_t DIR_LstAccDate;
	    uint16_t DIR_FstClusHI;
	    uint16_t DIR_WrtTime;
	    uint16_t DIR_WrtDate;
	    uint16_t DIR_FstClusLO;
	    uint32_t DIR_FileSize;
	};

vector <BPBinf*> all_BPB;
vector <uint16_t> all_FAT;
vector <DirectInf*> all_Dir;
vector <SVMDirectoryEntry*> all_SVM;
	 
}


//**********************************Thread STUFF *********************************
extern "C"
{
	class ThreadCB
	{
		public:
		ThreadCB();
		TVMThreadID tID;
		TVMThreadPriority tPriority;
		TVMThreadState tState;
		TVMMemorySize tStackSize;
		uint8_t* stackPtr;
		TVMThreadEntry tEntryPt;
		void* tEntryParam;
		SMachineContext tMachineContext;
		TVMTick tTick;
		int fileResult;
	};

	ThreadCB::ThreadCB()
	{
		tID =  0;
		tPriority = 0;
		tState = 0;
		tTick = 0;
	}
}

queue <ThreadCB*> low_queue;
queue <ThreadCB*> medium_queue;
queue <ThreadCB*> high_queue;
vector <ThreadCB*> waitingt;
vector <ThreadCB*> all_threads;

SMachineContextRef old_context;
SMachineContextRef new_context;
ThreadCB* curr_thread;

#define IDLE_THREAD_ID 0
#define MAIN_THREAD_ID 1
#define VM_THREAD_PRIORITY_LOWEST                 ((TVMThreadPriority)0x00)

//*****************************Mutex Stuff****************************
extern "C"
{
	class MutexCB
	{
		public:
		TVMMutexID mutexID;
		TVMThreadID mutexOwner;
		int lock;
		queue <ThreadCB*> low_mutex;
		queue <ThreadCB*> medium_mutex;
		queue <ThreadCB*> high_mutex;
	};
}
vector <MutexCB*> all_mutex;

TVMMutexID fatMutex;

//*************************Mem Pool stuff********************************************
typedef struct MemBlock
{
    uint8_t *base;
    TVMMemorySize length;
} MemBlock;

typedef unsigned int TVMMemoryPoolID, *TVMMemoryPoolIDRef;
typedef unsigned int TVMMemorySize, *TVMMemorySizeRef;

extern "C"
{
	class MemPool
	{
        public:
	   MemPool();
           TVMMemorySize memSize;
    	   TVMMemoryPoolID memID;
	   uint8_t* base;	   
	   TVMMemorySize free_space;

	   vector<MemBlock*> free_mem;
    	   vector<MemBlock*> alloc_mem;
	   
	};

    	MemPool::MemPool()
    	{
           memSize = 0;
           memID = 0;
           base = 0;
    	}
}

vector<MemPool*>all_mem;
void* sharedSpace;
const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;


//*******************************************************************


extern "C" void idle(void* param)
{
	while(1){}
}

extern "C" void pushPrioQ(ThreadCB *thread)
{
        if (thread->tPriority == VM_THREAD_PRIORITY_LOW)
	       low_queue.push(thread);
       	if (thread->tPriority == VM_THREAD_PRIORITY_NORMAL)
                medium_queue.push(thread);
        if (thread->tPriority == VM_THREAD_PRIORITY_HIGH)
                high_queue.push(thread);
}

extern "C" void Scheduler()
{
    if (!high_queue.empty())
	{
        if((high_queue.front()->tPriority == curr_thread->tPriority
        && curr_thread->tState != VM_THREAD_STATE_RUNNING)
		|| high_queue.front()->tPriority > curr_thread->tPriority)
		{
                if(curr_thread->tState == VM_THREAD_STATE_RUNNING)
				{
					curr_thread->tState = VM_THREAD_STATE_READY;
					pushPrioQ(curr_thread);
				}
				ThreadCB* popthread = high_queue.front();
                high_queue.pop();
                popthread->tState = VM_THREAD_STATE_RUNNING;
                old_context = &(curr_thread->tMachineContext);
                new_context = &(popthread->tMachineContext);
                curr_thread = popthread;
				MachineContextSwitch(old_context,new_context);
	    }
	}
    else if (!medium_queue.empty())
    {
	    if((medium_queue.front()->tPriority == curr_thread->tPriority
		&& curr_thread->tState != VM_THREAD_STATE_RUNNING)
		|| curr_thread->tState != VM_THREAD_STATE_RUNNING
		|| medium_queue.front()->tPriority > curr_thread->tPriority)
        {
                if(curr_thread->tState == VM_THREAD_STATE_RUNNING)
                {
                    curr_thread->tState = VM_THREAD_STATE_READY;
                    pushPrioQ(curr_thread);
                }
                ThreadCB* popthread = medium_queue.front();
                medium_queue.pop();
                popthread->tState = VM_THREAD_STATE_RUNNING;
                old_context = &(curr_thread->tMachineContext);
                new_context = &(popthread->tMachineContext);
                curr_thread = popthread;
                MachineContextSwitch(old_context,new_context);
		}
    }
    else if (!low_queue.empty())
    {
        if((low_queue.front()->tPriority == curr_thread->tPriority
        && curr_thread->tState != VM_THREAD_STATE_RUNNING)
        || curr_thread->tState != VM_THREAD_STATE_RUNNING
		|| low_queue.front()->tPriority > curr_thread->tPriority)
        {
                if(curr_thread->tState == VM_THREAD_STATE_RUNNING)
                {
                    curr_thread->tState = VM_THREAD_STATE_READY;
                    pushPrioQ(curr_thread);
                }
                ThreadCB* popthread = low_queue.front();
                low_queue.pop();
                popthread->tState = VM_THREAD_STATE_RUNNING;
                old_context = &(curr_thread->tMachineContext);
                new_context = &(popthread->tMachineContext);
                curr_thread = popthread;
                MachineContextSwitch(old_context,new_context);
        }
    }
	else if(curr_thread->tState != VM_THREAD_STATE_RUNNING)
	{

		volatile unsigned int oldID = curr_thread->tID;
    	curr_thread = all_threads[IDLE_THREAD_ID];
		old_context = &(curr_thread->tMachineContext);
		MachineContextSwitch(&(all_threads[oldID]->tMachineContext),&(all_threads[IDLE_THREAD_ID]->tMachineContext));

    }

}

extern "C" void Skeleton(void *param)
{
		MachineEnableSignals();
        TVMThreadEntry entryinf = ((ThreadCB*)param)->tEntryPt;
        entryinf(((ThreadCB*)param)->tEntryParam);
        VMThreadTerminate(((ThreadCB*)param)->tID);
}



extern "C" void sleepCallback(void *data)
{
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	for (unsigned int i = 0; i < waitingt.size(); i++)
	{
		if (waitingt[i]->tTick == 0)
		{
         		waitingt.erase(waitingt.begin()+i);

			if(waitingt[i]->tState!=VM_THREAD_STATE_DEAD)
			{
				waitingt[i]->tState = VM_THREAD_STATE_READY;
				pushPrioQ(waitingt[i]);
				Scheduler();
			}
		}
		waitingt[i]->tTick--;
	}
	MachineResumeSignals(&OldState);
}

extern "C" void fileCallBack(void* calldata, int result)
{
	((ThreadCB*)calldata)->fileResult = result;
	((ThreadCB*)calldata)->tState = VM_THREAD_STATE_READY;
	pushPrioQ((ThreadCB*)calldata);
	Scheduler();
}

extern "C" TVMMainEntry VMLoadModule(const char *module);

extern "C" TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, TVMMemorySize sharedsize, const char *mount, int argc, char *argv[])
{
	sharedsize = (sharedsize + 0x1000) & (~0x1000);
        sharedSpace = MachineInitialize(machinetickms,sharedsize);
        MachineEnableSignals();
	MachineRequestAlarm(tickms*1000, sleepCallback, NULL);
	
	ThreadCB* idle_thread = new ThreadCB;
        idle_thread->tID = 0;
        idle_thread->tState = VM_THREAD_STATE_READY;
        idle_thread->tEntryPt = idle;
        idle_thread->tEntryParam = NULL;
        idle_thread->tPriority = VM_THREAD_PRIORITY_LOWEST;
        idle_thread->tStackSize = 100000;
        idle_thread->stackPtr = new uint8_t[100000];
	all_threads.push_back(idle_thread);

        ThreadCB* main_thread = new ThreadCB;
        main_thread->tState = VM_THREAD_STATE_RUNNING;
        main_thread->tPriority = VM_THREAD_PRIORITY_NORMAL;
        main_thread->tID = all_threads.size();
        all_threads.push_back(main_thread);

        curr_thread = main_thread;

        MachineContextCreate(&(idle_thread->tMachineContext), Skeleton, (void*)idle_thread, idle_thread->stackPtr, idle_thread->tStackSize);

        TVMMainEntry mainentry = VMLoadModule(argv[0]);

 //************************************MemPool Stuff ********************************************
	MemPool* mainPool = new MemPool; 	
	mainPool->memSize = heapsize;
	mainPool->base = new uint8_t[heapsize];		  
	mainPool->free_space = heapsize;
	mainPool->memID = all_mem.size();

	MemBlock* mainChunk = new MemBlock;
	mainChunk->base = mainPool->base;
	mainChunk->length = mainPool->memSize;
	mainPool->free_mem.push_back(mainChunk);
	all_mem.push_back(mainPool);	

	MemPool* sharedPool = new MemPool;
	sharedPool->memSize = sharedsize;
	sharedPool->base = (uint8_t*)sharedSpace;
	sharedPool->free_space = sharedsize;
	sharedPool->memID = all_mem.size();
	all_mem.push_back(sharedPool);

	MemBlock* sharedChunk = new MemBlock;
	sharedChunk->base = sharedPool->base;
	sharedChunk->length = sharedPool->memSize;
	sharedPool->free_mem.push_back(sharedChunk);

//**************************************FAT Stuff ********************************************
	uint8_t *BPBImage;
	uint16_t *FATImage;
	uint8_t *DirImage;
	int fat_dir;

	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);

	VMMutexCreate(&fatMutex);
	//TMachineSignalState OldState;
	//MachineSuspendSignals(&OldState);

	curr_thread->tState = VM_THREAD_STATE_WAITING;
	MachineFileOpen(mount, O_RDWR, 0644, fileCallBack, curr_thread);
	Scheduler();

	fat_dir = curr_thread->fileResult;

//****************BPB********************
	VMMutexAcquire(fatMutex,VM_TIMEOUT_INFINITE);
	curr_thread->tState = VM_THREAD_STATE_WAITING;
    void* BPBPointer;
	VMMemoryPoolAllocate(all_mem[1]->memID, 512, (void**)&BPBPointer);
	MachineFileRead(fat_dir, BPBPointer, 512, fileCallBack, (void*)curr_thread);
    Scheduler();   

    MachineResumeSignals(&OldState);   

	BPBImage = (uint8_t*)BPBPointer;
	BPBinf* newBPB = new BPBinf;

	newBPB->BPB_BytsPerSec = (((uint16_t)BPBImage[12]) << 8) + ((uint8_t)BPBImage[11]);
	newBPB->BPB_SecPerClus = ((uint8_t)BPBImage[13]);
    newBPB->BPB_RsvdSecCnt = (((uint16_t)BPBImage[15]) << 8) + ((uint8_t)BPBImage[14]); 
    newBPB->BPB_NumFATs = ((uint8_t)BPBImage[16]);
	newBPB->BPB_RootEntCnt = (((uint16_t)BPBImage[18]) << 8) + ((uint8_t)BPBImage[17]);
	newBPB->BPB_TotSec16 = (((uint16_t)BPBImage[20]) << 8) + ((uint8_t)BPBImage[19]);
	newBPB->BPB_Media = ((uint8_t)BPBImage[21]);
	newBPB->BPB_FATSz16 = (((uint16_t)BPBImage[23]) << 8) + ((uint8_t)BPBImage[22]);
	newBPB->BPB_SecPerTrk = (((uint16_t)BPBImage[25]) << 8) + ((uint8_t)BPBImage[24]);
	newBPB->BPB_NumHeads = (((uint16_t)BPBImage[27]) << 8) + ((uint8_t)BPBImage[26]);
	newBPB->BPB_HiddSec = (((uint32_t)BPBImage[31]) << 24) + (((uint32_t)BPBImage[30]) << 16) + (((uint16_t)BPBImage[29]) << 8) + ((uint8_t)BPBImage[28]);
	newBPB->BPB_TotSec32 = (((uint32_t)BPBImage[35]) << 24) + (((uint32_t)BPBImage[34]) << 16) + (((uint16_t)BPBImage[33]) << 8) + ((uint8_t)BPBImage[32]);
	newBPB->BS_DrvNum = ((uint8_t)BPBImage[36]);
	newBPB->BS_Reserved1 = ((uint8_t)BPBImage[37]);
	newBPB->BS_BootSig = ((uint8_t)BPBImage[38]);

	all_BPB.push_back(newBPB);

	VMMemoryPoolDeallocate(all_mem[1]->memID, BPBPointer);
	VMMutexRelease(fatMutex);

   /* printf("%.04X \n", newBPB->BPB_BytsPerSec);
    printf("%.02X \n", newBPB->BPB_SecPerClus);
    printf("%.04X \n", newBPB->BPB_RsvdSecCnt);
    printf("%.02X \n", newBPB->BPB_NumFATs);
    printf("%.04X \n", newBPB->BPB_RootEntCnt);
    printf("%.04X \n", newBPB->BPB_TotSec16);
    printf("%.02X \n", newBPB->BPB_Media);
    printf("%.04X \n", newBPB->BPB_FATSz16);
    printf("%.04X \n", newBPB->BPB_SecPerTrk);
    printf("%.04X \n", newBPB->BPB_NumHeads);
    printf("%.02X \n", newBPB->BS_DrvNum);
    printf("%.02X \n", newBPB->BS_Reserved1);
    printf("%.02X \n", newBPB->BS_BootSig);*/
    uint16_t FirstRootSector = newBPB->BPB_RsvdSecCnt + newBPB->BPB_NumFATs * newBPB->BPB_FATSz16;
    uint16_t RootDirectorySectors = (newBPB->BPB_RootEntCnt * 32) / 512;
    uint16_t FirstDataSector = FirstRootSector + RootDirectorySectors;
    uint16_t ClusterCount = (newBPB->BPB_TotSec32 - FirstDataSector) / newBPB->BPB_SecPerClus;

//******************FAT*****************
	VMMutexAcquire(fatMutex, VM_TIMEOUT_INFINITE);
	curr_thread->tState = VM_THREAD_STATE_WAITING;

    MachineSuspendSignals(&OldState);
	MachineFileSeek(fat_dir, 512*(newBPB->BPB_RsvdSecCnt), 0, fileCallBack, (void*)curr_thread);
    Scheduler();   

    MachineResumeSignals(&OldState);    		

	curr_thread->tState = VM_THREAD_STATE_WAITING;
    void* FATPointer;
    MachineSuspendSignals(&OldState);
	VMMemoryPoolAllocate(all_mem[1]->memID, 512, (void**)&FATPointer);
	MachineFileRead(fat_dir, FATPointer, 512, fileCallBack, (void*)curr_thread);
    Scheduler(); 
    MachineResumeSignals(&OldState);  

    FATImage = (uint16_t*)FATPointer;
    for(int j = 0; j < 512; ++j) 
    	all_FAT.push_back(((uint16_t *)FATImage)[j]);
		//printf("%.04X ", ((uint16_t *)FATImage)[j]);
	VMMemoryPoolDeallocate(all_mem[1]->memID, FATPointer);  
	VMMutexRelease(fatMutex);

//*********************Root********************
	MachineSuspendSignals(&OldState);
	VMMutexAcquire(fatMutex, VM_TIMEOUT_INFINITE);
	curr_thread->tState = VM_THREAD_STATE_WAITING;
	MachineFileSeek(fat_dir, (512*FirstRootSector), 0, fileCallBack, (void*)curr_thread);
    Scheduler();   

    for (unsigned int k = 0; k < RootDirectorySectors; k++)
    { 
		curr_thread->tState = VM_THREAD_STATE_WAITING;
	    void* DirPointer;

		VMMemoryPoolAllocate(all_mem[1]->memID, 512, (void**)&DirPointer);
		MachineFileRead(fat_dir, DirPointer, 512, fileCallBack, (void*)curr_thread);
	    Scheduler(); 

	    DirImage = (uint8_t*)DirPointer;
	    //for(int j = 0; j < 512; ++j) 
		//	printf("%.04X ", ((uint16_t *)DirImage)[j]);
		for (unsigned int n = 0; n < 512/32; n++)
		{
			if(DirImage[11+(n*32)] == (VM_FILE_SYSTEM_ATTR_READ_ONLY  | VM_FILE_SYSTEM_ATTR_HIDDEN | VM_FILE_SYSTEM_ATTR_SYSTEM  | VM_FILE_SYSTEM_ATTR_VOLUME_ID))
			{}
			else if (DirImage[0] == (0xE5))
			{}
			else
			{
				DirectInf* newDirect = new DirectInf;
				memcpy(&(newDirect->DIR_Name), &(DirImage[n*32]), 11);
				/*for(int m = 0; m < 11; m++) 
				{				
            		printf("%c", newDirect->DIR_Name[m]);
        		}
        		cerr << endl << all_SVM.size() << endl;
        		cerr.flush();*/

        		newDirect->DIR_Name[11] = '\0';
				newDirect->DIR_Attr = ((uint8_t)DirImage[11+(n*32)]);
				newDirect->DIR_NTRes = ((uint8_t)DirImage[12+(n*32)]);;
				newDirect->DIR_CrtTimeTenth = ((uint8_t)DirImage[13+(n*32)]);;
				newDirect->DIR_CrtTime = (((uint16_t)DirImage[15+(n*32)]) << 8) + ((uint8_t)DirImage[14+(n*32)]);
				newDirect->DIR_CrtDate = (((uint16_t)DirImage[17+(n*32)]) << 8) + ((uint8_t)DirImage[16+(n*32)]);
				newDirect->DIR_LstAccDate = (((uint16_t)DirImage[19+(n*32)]) << 8) + ((uint8_t)DirImage[18+(n*32)]);
				newDirect->DIR_FstClusHI = (((uint16_t)DirImage[21+(n*32)]) << 8) + ((uint8_t)DirImage[20+(n*32)]);
				newDirect->DIR_WrtTime = (((uint16_t)DirImage[23+(n*32)]) << 8) + ((uint8_t)DirImage[22+(n*32)]);
				newDirect->DIR_WrtDate = (((uint16_t)DirImage[25+(n*32)]) << 8) + ((uint8_t)DirImage[24+(n*32)]);
				newDirect->DIR_FstClusLO = (((uint16_t)DirImage[27+(n*32)]) << 8) + ((uint8_t)DirImage[26+(n*32)]);
				newDirect->DIR_FileSize = (((uint32_t)DirImage[31+(n*32)]) << 24) + (((uint32_t)DirImage[30+(n*32)]) << 16) + (((uint16_t)DirImage[29+(n*32)]) << 8) + ((uint8_t)DirImage[28+(n*32)]);

				SVMDirectoryEntry* newDir = new SVMDirectoryEntry;

				newDir->DSize = newDirect->DIR_FileSize;
				newDir->DAttributes = newDirect->DIR_Attr;
				memcpy(&(newDir->DShortFileName), &(newDirect->DIR_Name), 12);

				newDir->DCreate.DYear = newDirect->DIR_CrtDate;
					newDir->DCreate.DYear = 1980 + ((newDir->DCreate.DYear) >> 9);
				newDir->DCreate.DMonth = newDirect->DIR_CrtDate & 480;
					newDir->DCreate.DMonth = (newDir->DCreate.DMonth) >> 5;
				newDir->DCreate.DDay = newDirect->DIR_CrtDate & 31;
				newDir->DCreate.DHour = newDirect->DIR_CrtTime;
					newDir->DCreate.DHour = (newDir->DCreate.DHour) >> 11;
				newDir->DCreate.DMinute = newDirect->DIR_CrtTime & 2016;
					newDir->DCreate.DMinute = (newDir->DCreate.DMinute) >> 5;
				newDir->DCreate.DSecond = newDirect->DIR_CrtTime & 31;
				newDir->DCreate.DHundredth = newDirect->DIR_CrtTimeTenth;
				
				newDir->DAccess.DYear = newDirect->DIR_LstAccDate;
					newDir->DAccess.DYear = 1980 + ((newDir->DAccess.DYear) >> 9);
				newDir->DAccess.DMonth = newDirect->DIR_LstAccDate & 480;
					newDir->DAccess.DMonth = (newDir->DAccess.DMonth) >> 5;
				newDir->DAccess.DDay = newDirect->DIR_LstAccDate & 31;
				
				newDir->DModify.DYear = newDirect->DIR_WrtDate;
					newDir->DModify.DYear = 1980 + ((newDir->DAccess.DYear) >> 9);
				newDir->DModify.DMonth = newDirect->DIR_WrtDate & 480;
					newDir->DModify.DMonth = (newDir->DAccess.DMonth) >> 5;
				newDir->DModify.DDay = newDirect->DIR_WrtDate & 31;
				newDir->DModify.DHour = newDirect->DIR_WrtTime;
					newDir->DModify.DHour = (newDir->DModify.DHour) >> 11;
				newDir->DModify.DMinute = newDirect->DIR_WrtTime & 2016;
					newDir->DModify.DMinute = (newDir->DModify.DMinute) >> 5;
				newDir->DModify.DSecond = newDirect->DIR_WrtTime & 31;
				newDir->DModify.DHundredth = newDirect->DIR_CrtTimeTenth;

				if(newDir->DShortFileName[0] >= 0x30 && newDir->DShortFileName[0] <= 0x7A)
					rootCount++;
				all_SVM.push_back(newDir);

/*
				cerr << "Create Date Time: " << (unsigned int)newDir->DCreate.DMonth << "/"
				<< (unsigned int)newDir->DCreate.DDay << "/" << (unsigned int)newDir->DCreate.DYear
				<< "  " << (unsigned int)newDir->DCreate.DHour << ":" << (unsigned int)newDir->DCreate.DMinute
				<< ":" << (unsigned int)newDir->DCreate.DSecond << endl;
				cerr.flush();

				printf("%.02X \n", newDirect->DIR_Attr);
				printf("%.02X \n", newDirect->DIR_NTRes);
				printf("%.02X \n", newDirect->DIR_NTRes);
				printf("%.04X \n", newDirect->DIR_CrtTime);
				printf("%.04X \n", newDirect->DIR_CrtDate);
				printf("%.04X \n", newDirect->DIR_LstAccDate);
				printf("%.04X \n", newDirect->DIR_FstClusHI);
				printf("%.04X \n", newDirect->DIR_WrtTime);
				printf("%.04X \n", newDirect->DIR_WrtDate);
				printf("%.04X \n", newDirect->DIR_FstClusLO);
				printf("%.08X \n", newDirect->DIR_FileSize);
*/

			}
		}
		VMMemoryPoolDeallocate(all_mem[1]->memID, DirPointer);  
	}

	VMMutexRelease(fatMutex);
    MachineResumeSignals(&OldState);

        if(mainentry == NULL)
            return VM_STATUS_FAILURE;

        mainentry(argc,argv);

		MachineTerminate();
		return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
{
	if (entry == NULL || tid == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);

	ThreadCB* thread = new ThreadCB;
        VMMemoryPoolAllocate(all_mem[0]->memID,memsize,(void**)&(thread->stackPtr));
	*tid = all_threads.size();
	thread->tID = all_threads.size();
	thread->tState = VM_THREAD_STATE_DEAD;
	thread->tEntryParam = param;
	thread->tEntryPt = entry;
	thread->tStackSize = memsize;
	thread->tPriority = prio;
	all_threads.push_back(thread);

	MachineResumeSignals(&OldState);

	return VM_STATUS_SUCCESS;
}


extern "C" TVMStatus VMThreadDelete(TVMThreadID thread)
{
	if(thread >= all_threads.size() || all_threads[thread] == NULL)
		return VM_STATUS_ERROR_INVALID_ID;

	if( all_threads[thread]->tState == VM_THREAD_STATE_DEAD)
	{
		all_threads[thread] = NULL;
	    return VM_STATUS_SUCCESS;
	}
	else
		return VM_STATUS_ERROR_INVALID_STATE;

}

extern "C" TVMStatus VMThreadActivate(TVMThreadID thread)
{
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);

	if(thread >= all_threads.size() || all_threads[thread] == NULL)
	{
		MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(all_threads[thread]->tState != VM_THREAD_STATE_DEAD)
    {
        MachineResumeSignals(&OldState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }

	MachineContextCreate(&(all_threads[thread]->tMachineContext), Skeleton, (void*)all_threads[thread], all_threads[thread]->stackPtr, all_threads[thread]->tStackSize);

	all_threads[thread]->tState = VM_THREAD_STATE_READY;

	pushPrioQ(all_threads[thread]);

	Scheduler();

	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMThreadTerminate(TVMThreadID thread)
{
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);

    if(thread >= all_threads.size())
    {
        MachineResumeSignals(&OldState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if(all_threads[thread]->tState == VM_THREAD_STATE_DEAD)
    {
        MachineResumeSignals(&OldState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
	all_threads[thread]->tState = VM_THREAD_STATE_DEAD;

	MachineResumeSignals(&OldState);

	Scheduler();

	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMThreadID(TVMThreadIDRef threadref)
{
	if(threadref == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	*threadref = curr_thread->tID;
	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
{
	if (stateref == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	if (thread >= all_threads.size())
		return VM_STATUS_ERROR_INVALID_ID;
	*stateref = all_threads[thread]->tState;

	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMThreadSleep(TVMTick tick)
{
    TMachineSignalState OldState;
    MachineSuspendSignals(&OldState);
	if (tick == VM_TIMEOUT_INFINITE)
	{
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	if(tick == VM_TIMEOUT_IMMEDIATE)
		Scheduler();

    if(curr_thread->tState != VM_THREAD_STATE_RUNNING)
    {
        if (curr_thread->tPriority == VM_THREAD_PRIORITY_LOW);
            low_queue.pop();
        if (curr_thread->tPriority == VM_THREAD_PRIORITY_NORMAL)
            medium_queue.pop();
        if (curr_thread->tPriority == VM_THREAD_PRIORITY_HIGH)
            high_queue.pop();
    }

	curr_thread->tTick = tick;
	curr_thread->tState = VM_THREAD_STATE_WAITING;

	waitingt.push_back(curr_thread);
	Scheduler();

	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
{
        if(data == NULL || length == NULL)
                return VM_STATUS_ERROR_INVALID_PARAMETER;

        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        curr_thread->tState = VM_THREAD_STATE_WAITING;
	
	void* basePointer;
	int tempLength = *length;
	while(tempLength > 0)
	{
		VMMemoryPoolAllocate(all_mem[1]->memID, 512, (void**)&basePointer);

		memcpy(basePointer, data, tempLength>512 ? 512 : tempLength);

		MachineFileWrite(filedescriptor,basePointer,
		tempLength>512 ? 512 : tempLength, fileCallBack, (void*)curr_thread);
		
		Scheduler();

		VMMemoryPoolDeallocate(all_mem[1]->memID, basePointer);
		tempLength -= 512;
		data += 512;
	}


        MachineResumeSignals(&OldState);
        if(curr_thread->fileResult < 0)
	        return VM_STATUS_FAILURE;
	*length = curr_thread->fileResult;
	return VM_STATUS_SUCCESS;
}


extern "C" TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
{
    if(filedescriptor == NULL || filename == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    TMachineSignalState OldState;
    MachineSuspendSignals(&OldState);
    /*
    int found = 0;
    int dot = 0;
    int spacing = 0;
    int spaceHold = 0;
    char temp[11];
    int typeCount = 0;

    for(int k = 0; k < 11; k++)
    {
    	if(filename[k] >= 0x61 && filename[k] <= 0x7A)
    		toupper(filename[k]);
    }

    for (int i = 0; i < rootCount; i++)
    {
    	for(int n = 0; n < 11; n++)
    	{
    		if(!(all_SVM[i]->DShortFileName[n] >= 0x30 && all_SVM[i]->DShortFileName[n] <= 0x5A))
    		{
    			if(dot != 1)
    			{	
    				dot == 1;
    				all_SVM[i]->DShortFileName[n] = 0x2E;
    			}
    			else if(spacing != 1)
    			{
    				spaceHold = n;
    				spacing = 1;
    			}
    		}
    		else if (dot != 1)
    		{
    			temp[n] = all_SVM[i]->DShortFileName[n];
    		}
    		else if(dot == 1)
    		{
    			typeCount++;
    		}
    	}
    	if(spacing == 1)
    	{
    		for (int idk = typeCount; typeCount > 0; idk--)
    		{	
    			temp[n] = temp[10-idk];
    			n++;
    		}
    	}

	    for(int j = 0; j < filename.size(); j++)
	    {
	    	if(toupper(filename[j]) != all_SVM[i]->DShortFileName[j])
	    	{
	    		break;
	    	}
	    	else if(j == filename.size())
	    	{
	    		found = 1;
	    		break;
	    	}
	    }
	    if(found == 1)
	  		break;
    }
*/
    curr_thread->tState = VM_THREAD_STATE_WAITING;

    MachineFileOpen(filename,  flags, mode, fileCallBack, (void*)curr_thread);

    Scheduler();
	MachineResumeSignals(&OldState);

	if(curr_thread->fileResult < 0)
                return VM_STATUS_FAILURE;
	*filedescriptor = curr_thread->fileResult;
      	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMFileClose(int filedescriptor)
{
  	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);

        curr_thread->tState = VM_THREAD_STATE_WAITING;

        MachineFileClose(filedescriptor, fileCallBack, (void*)curr_thread);

        Scheduler();

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
{
        if(data == NULL || length == NULL)
            return VM_STATUS_ERROR_INVALID_PARAMETER;

        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        curr_thread->tState = VM_THREAD_STATE_WAITING;
        void* basePointer;
        int tempLength = *length;
        while(tempLength > 0)
        {       
		
                VMMemoryPoolAllocate(all_mem[1]->memID, 512, (void**)&basePointer);
		MachineFileRead(filedescriptor,basePointer,
		tempLength>512 ? 512 : tempLength, fileCallBack, (void*)curr_thread);
                Scheduler();        
		memcpy(data, basePointer, tempLength>512 ? 512 : tempLength);

		VMMemoryPoolDeallocate(all_mem[1]->memID, basePointer);
                tempLength -= 512;
                data+= 512;
        }


        MachineResumeSignals(&OldState);
        if(curr_thread->fileResult < 0)
                return VM_STATUS_FAILURE;
	*length = curr_thread->fileResult;
        return VM_STATUS_SUCCESS;
}


extern "C" TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
{
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

        curr_thread->tState = VM_THREAD_STATE_WAITING;

        MachineFileSeek(filedescriptor, offset, whence, fileCallBack, (void*)curr_thread);

        Scheduler();

        MachineResumeSignals(&OldState);
        if(curr_thread->fileResult < 0)
        	return VM_STATUS_FAILURE;
        *newoffset = curr_thread->fileResult;
        return VM_STATUS_SUCCESS;
}


extern "C" TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
{
    	if(mutexref == NULL)
        	return VM_STATUS_ERROR_INVALID_PARAMETER;
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState); 

   	*mutexref = all_mutex.size();
	MutexCB* newMutex = new MutexCB;
	newMutex->mutexID = all_mutex.size();
	newMutex->lock = 0; 
   	all_mutex.push_back(newMutex);

	MachineResumeSignals(&OldState);
    	return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMMutexDelete(TVMMutexID mutex)
{
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

	unsigned int i;
	for (i = 0; i < all_mutex.size(); i++)
		if (all_mutex[i]->mutexID == mutex)
			break;

	if( i == all_mutex.size())
	{
	        MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	else if (all_mutex[i]->lock == 1)
	{
	        MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	else
		all_mutex.erase(all_mutex.begin() + i);

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
}

extern "C" TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
{
	if(ownerref == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

	unsigned int i;
        for(i = 0; i < all_mutex.size(); i++)
                if(all_mutex[i]->mutexID == mutex)
                        break;

        if( i == all_mutex.size())
        {
                MachineResumeSignals(&OldState);
                return VM_STATUS_ERROR_INVALID_ID;
        }
	if(all_mutex[i]->lock == 1)
	{
		*ownerref = all_mutex[i]->mutexOwner;
	        MachineResumeSignals(&OldState);
        	return VM_STATUS_SUCCESS;
	}
	else
	{
	        MachineResumeSignals(&OldState);
		return VM_THREAD_ID_INVALID;
	}
}
extern "C" TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
{
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

	unsigned int i;
	for(i = 0; i < all_mutex.size(); i++)
		if(all_mutex[i]->mutexID == mutex)
			break;

	if(i == all_mutex.size())
	{
        MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_ID;
	}

		curr_thread->tState = VM_THREAD_STATE_WAITING;
        if(all_mutex[i]->lock == 0)
        {
        	all_mutex[i]->lock = 1;
            all_mutex[i]->mutexOwner = curr_thread->tID;
			MachineResumeSignals(&OldState);
            return VM_STATUS_SUCCESS;
        }

		if(curr_thread->tPriority == VM_THREAD_PRIORITY_HIGH)
		{
			all_mutex[i]->high_mutex.push(curr_thread);
		}
		else if(curr_thread->tPriority == VM_THREAD_PRIORITY_NORMAL)
		{
			all_mutex[i]->medium_mutex.push(curr_thread);
		}
		else
		{
			all_mutex[i]->low_mutex.push(curr_thread);
		}
		Scheduler();
	

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
}


extern "C" TVMStatus VMMutexRelease(TVMMutexID mutex)
{
    TMachineSignalState OldState;
    MachineSuspendSignals(&OldState);

	unsigned int i;
	for(i = 0; i < all_mutex.size(); i++)
		if(all_mutex[i]->mutexID == mutex)
			break;
	if(i == all_mutex.size())
	{
        MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_ID;
	}

	all_mutex[i]->lock = 0;

	ThreadCB* pop;
       
    if(!all_mutex[i]->high_mutex.empty())
    {
            all_mutex[i]->lock = 1;
            pop = all_mutex[i]->high_mutex.front();
	        all_mutex[i]->high_mutex.pop();
            pop->tState = VM_THREAD_STATE_READY;
            all_mutex[i]->mutexOwner = pop->tID;
            pushPrioQ(pop);
    }
    else if(!all_mutex[i]->medium_mutex.empty())
    {
            all_mutex[i]->lock = 1;
            pop= all_mutex[i]->medium_mutex.front();
	        all_mutex[i]->medium_mutex.pop();
            pop->tState = VM_THREAD_STATE_READY;
            all_mutex[i]->mutexOwner = pop->tID;
            pushPrioQ(pop);
    }
    else if(!all_mutex[i]->low_mutex.empty())
    { 
            all_mutex[i]->lock = 1;
            pop = all_mutex[i]->low_mutex.front();
	        all_mutex[i]->low_mutex.pop();
            pop->tState = VM_THREAD_STATE_READY;
            all_mutex[i]->mutexOwner = pop->tID;
            pushPrioQ(pop);
    }
    if(all_mutex[i]->lock == 1)
        if(pop->tPriority > curr_thread->tPriority)        
            Scheduler();       

    MachineResumeSignals(&OldState);
    return VM_STATUS_SUCCESS;
}



/////////////////////////////VMMemoryPool Functions//////////////////////////////////////
TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
{
	if ((all_mem[memory] == NULL) || (size == 0) || (pointer == NULL))
        	return VM_STATUS_ERROR_INVALID_PARAMETER;

        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
	unsigned int i;
	MemBlock* newBlock = new MemBlock;
	newBlock->length = (size + 0x3F) & (~0x3F);
	for (i = 0; i<(all_mem[memory]->free_mem).size(); i++) 
	{
            if(((all_mem[memory])->free_mem[i])->length >= newBlock->length)
		break;	
	}

	if (((all_mem[memory])->free_mem[i])->length < newBlock->length)
        {
	   MachineResumeSignals(&OldState);
           return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
	}

	*pointer = (all_mem[memory]->free_mem[i])->base;
	newBlock->base = (all_mem[memory]->free_mem[i])->base;
	((all_mem[memory])->alloc_mem).push_back(newBlock);
	((all_mem[memory])->free_mem[i])->base += newBlock->length;
	((all_mem[memory])->free_mem[i])->length -= newBlock->length;
       
	(all_mem[memory])->free_space -= newBlock->length;

        MachineResumeSignals(&OldState);  
      	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
{
	if((base == NULL) || (memory == NULL) || (size == 0))
        	return VM_STATUS_ERROR_INVALID_PARAMETER;
        
	TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
	*memory = all_mem.size();
        MemPool* newMem = new MemPool;
	newMem->memID = all_mem.size();
        newMem->base = (uint8_t*)base;
	newMem->memSize = size;
	newMem->free_space = size;
	MemBlock* Chunk = new MemBlock;
        Chunk->base = newMem->base;
        Chunk->length = newMem->memSize;
        newMem->free_mem.push_back(Chunk);
	all_mem.push_back(newMem);
        

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
{
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);
	

	if (all_mem[memory] == NULL)
	{
                MachineResumeSignals(&OldState);
                return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
    	if (!all_mem[memory]->alloc_mem.empty())
    	{
                MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_STATE;
    	}
    	else 
    	{

		delete all_mem[memory];
 		all_mem[memory] = NULL;
	       	MachineResumeSignals(&OldState);
        	return VM_STATUS_SUCCESS;    	
	}
    
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
{
	if ((all_mem[memory] == NULL) || (bytesleft == NULL))
		return VM_STATUS_ERROR_INVALID_ID;
	TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

	*bytesleft = all_mem[memory]->free_space;

        MachineResumeSignals(&OldState);
        return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
{
	if ((all_mem[memory] == NULL) || (pointer == NULL))
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	
        TMachineSignalState OldState;
        MachineSuspendSignals(&OldState);

	MemPool* temp_pool = all_mem[memory];
	unsigned int i;
	for (i = 0; i < temp_pool->alloc_mem.size(); i++)
	{

		if(temp_pool->alloc_mem[i]!=NULL)
		{
			if ((temp_pool->alloc_mem[i])->base == (uint8_t*)pointer)
			break;
		}
	}	

	if (i == temp_pool->alloc_mem.size())
	{
		MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	int replaced = 0;
	unsigned int j;
	for(j = 0; j < temp_pool->free_mem.size(); j++)
	{
		replaced = 0;
		if (((temp_pool->free_mem[j])->base) + ((temp_pool->free_mem[j])->length) 
		== ((temp_pool->alloc_mem[i])->base))
		{
			((temp_pool->free_mem[j])->length) += (temp_pool->alloc_mem[i])->length;
			replaced = 1;
			break;
		}
		if(temp_pool->free_mem[j]->length !=0)
		{
			if (((temp_pool->alloc_mem[i])->base) + ((temp_pool->alloc_mem[i])->length) 
			== (temp_pool->free_mem[j])->base)
			{
				((temp_pool->free_mem[j])->length) += (temp_pool->alloc_mem[i])->length;
                        	((temp_pool->free_mem[j])->base) = (temp_pool->alloc_mem[i])->base;
				replaced = 1;
				break;	
			}
		}
	}
	if (replaced == 0)
		(temp_pool->free_mem).push_back(temp_pool->alloc_mem[i]);
	
	temp_pool->free_space += temp_pool->alloc_mem[i]->length;
        temp_pool->alloc_mem[i] = NULL;
	temp_pool->alloc_mem.erase(temp_pool->alloc_mem.begin()+i);		
	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;
}

/*
TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor);
TVMStatus VMDirectoryClose(int dirdescriptor);
TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent);
TVMStatus VMDirectoryRewind(int dirdescriptor);
TVMStatus VMDirectoryCurrent(char *abspath);
TVMStatus VMDirectoryChange(const char *path);
TVMStatus VMDirectoryCreate(const char *dirname);
TVMStatus VMDirectoryUnlink(const char *path);
*/




