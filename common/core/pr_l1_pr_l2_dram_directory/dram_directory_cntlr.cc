using namespace std;

#include "dram_directory_cntlr.h"
#include "log.h"
#include "memory_manager.h"

namespace PrL1PrL2DramDirectory
{

DramDirectoryCntlr::DramDirectoryCntlr(MemoryManager* memory_manager,
      UInt32 dram_directory_total_entries,
      UInt32 dram_directory_associativity,
      UInt32 cache_block_size,
      UInt32 dram_directory_max_num_sharers,
      UInt32 dram_directory_max_hw_sharers,
      string dram_directory_type_str,
      DramCntlr* dram_cntlr):
   m_memory_manager(memory_manager),
   m_dram_cntlr(dram_cntlr),
   m_cache_block_size(cache_block_size)
{
   m_dram_directory_cache = new DramDirectoryCache(
         dram_directory_type_str,
         dram_directory_total_entries,
         dram_directory_associativity,
         cache_block_size,
         dram_directory_max_hw_sharers,
         dram_directory_max_num_sharers);
   m_dram_directory_req_queue_list = new ReqQueueList();
}

DramDirectoryCntlr::~DramDirectoryCntlr()
{
   delete m_dram_directory_cache;
   delete m_dram_directory_req_queue_list;
}

void
DramDirectoryCntlr::handleMsgFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg, UInt64 msg_time)
{
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->m_msg_type;

   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REQ:
      case ShmemMsg::SH_REQ:

         {
            IntPtr address = shmem_msg->m_address;
            
            // Add request onto a queue
            ShmemReq* shmem_req = new ShmemReq(shmem_msg, sender, msg_time);
            m_dram_directory_req_queue_list->enqueue(address, shmem_req);
            if (m_dram_directory_req_queue_list->size(address) == 1)
            {
               if (shmem_msg_type == ShmemMsg::EX_REQ)
                  processExReqFromL2Cache(shmem_req);
               else // (shmem_msg_type == ShmemMsg::SH_REQ)
                  processShReqFromL2Cache(shmem_req);
            }
         }
         break;

      case ShmemMsg::INV_REP:
         processInvRepFromL2Cache(sender, shmem_msg);
         break;

      case ShmemMsg::FLUSH_REP:
         processFlushRepFromL2Cache(sender, shmem_msg);
         break;

      case ShmemMsg::WB_REP:
         processWbRepFromL2Cache(sender, shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized Shmem Msg Type: %u", shmem_msg_type);
         break;
   }
}

void
DramDirectoryCntlr::processNextReqFromL2Cache(IntPtr address)
{
   LOG_PRINT("Start processNextReqFromL2Cache(0x%x)", address);

   assert(m_dram_directory_req_queue_list->size(address) >= 1);
   LOG_PRINT("Starting to deque a finished shmem req for address(0x%x)", address);
   m_dram_directory_req_queue_list->dequeue(address);
   LOG_PRINT("Finished dequeing an shmem req for address(0x%x)", address);

   if (! m_dram_directory_req_queue_list->empty(address))
   {
      LOG_PRINT("A new shmem req for address(0x%x) found", address);
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);
      if (shmem_req->getShmemMsg()->m_msg_type == ShmemMsg::EX_REQ)
         processExReqFromL2Cache(shmem_req);
      else
         processShReqFromL2Cache(shmem_req);
   }
   LOG_PRINT("End processNextReqFromL2Cache(0x%x)", address);
}

void
DramDirectoryCntlr::processExReqFromL2Cache(ShmemReq* shmem_req)
{
   IntPtr address = shmem_req->getShmemMsg()->m_address;
   core_id_t sender = shmem_req->getSender();
   
   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();

   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch (curr_dstate)
   {
      case DirectoryState::EXCLUSIVE:
         getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REQ, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, directory_entry->getOwner(), address);
         break;
   
      case DirectoryState::SHARED:

         {
            pair<bool, vector<SInt32> > sharers_list_pair = directory_entry->getSharersList();
            if (sharers_list_pair.first == true)
            {
               // Broadcast Invalidation Request to all cores 
               // (irrespective of whether they are sharers or not)
               getMemoryManager()->broadcastMsg(ShmemMsg::INV_REQ, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, address);
            }
            else
            {
               // Send Invalidation Request to only a specific set of sharers
               for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
               {
                  getMemoryManager()->sendMsg(ShmemMsg::INV_REQ, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, sharers_list_pair.second[i], address);
               }
            }
         }
         break;
   
      case DirectoryState::UNCACHED:

         {
            // Modifiy the directory entry contents
            pair<bool, SInt32> add_result = directory_entry->addSharer(sender);
            assert(add_result.first == true);
            directory_entry->setOwner(sender);
            directory_block_info->setDState(DirectoryState::EXCLUSIVE);

            // Get the data from DRAM
            // This could be directly forwarded to the cache or passed
            // through the Dram Directory Controller
            Byte data_buf[getCacheBlockSize()];
            m_dram_cntlr->getDataFromDram(address, data_buf);

            getMemoryManager()->sendMsg(ShmemMsg::EX_REP, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, sender, address, data_buf, getCacheBlockSize());
         }

         // Process the next request from another L2 Cache(??)
         processNextReqFromL2Cache(address);

         break;
   
      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }
}

void
DramDirectoryCntlr::processShReqFromL2Cache(ShmemReq* shmem_req)
{
   IntPtr address = shmem_req->getShmemMsg()->m_address;
   core_id_t sender = shmem_req->getSender();

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();

   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch (curr_dstate)
   {
      case DirectoryState::EXCLUSIVE:
         getMemoryManager()->sendMsg(ShmemMsg::WB_REQ, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, directory_entry->getOwner(), address);
         break;
   
      case DirectoryState::SHARED:

         {
            pair<bool, SInt32> add_result = directory_entry->addSharer(sender);
            if (add_result.first == false)
            {
               // Send a message to another sharer to invalidate that
               getMemoryManager()->sendMsg(ShmemMsg::INV_REQ, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, add_result.second, address);
            }
            else
            {
               Byte data_buf[getCacheBlockSize()];
               m_dram_cntlr->getDataFromDram(address, data_buf);      
               getMemoryManager()->sendMsg(ShmemMsg::SH_REP, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, sender, address, data_buf, getCacheBlockSize());

               // Process Next Request
               processNextReqFromL2Cache(address);
            }
         }
         break;

      case DirectoryState::UNCACHED:

         {
            // Modifiy the directory entry contents
            pair<bool, SInt32> add_result = directory_entry->addSharer(sender);
            assert(add_result.first == true);
            directory_block_info->setDState(DirectoryState::SHARED);

            // Get the data from DRAM
            // This could be directly forwarded to the cache or passed
            // through the Dram Directory Controller
            Byte data_buf[getCacheBlockSize()];
            m_dram_cntlr->getDataFromDram(address, data_buf);
         
            getMemoryManager()->sendMsg(ShmemMsg::SH_REP, MemComponent::DRAM_DIR, MemComponent::L2_CACHE, sender, address, data_buf, getCacheBlockSize());

            // Process Next Request
            processNextReqFromL2Cache(address);
         }
         break;
   
      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }
}

void
DramDirectoryCntlr::processInvRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->m_address;

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   assert(directory_block_info->getDState() == DirectoryState::SHARED);

   directory_entry->removeSharer(sender);
   if (directory_entry->numSharers() == 0)
   {
      directory_block_info->setDState(DirectoryState::UNCACHED);
   }

   if (m_dram_directory_req_queue_list->size(address) > 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);
      if (shmem_req->getShmemMsg()->m_msg_type == ShmemMsg::EX_REQ)
      {
         // An ShmemMsg::EX_REQ caused the invalidation
         if (directory_block_info->getDState() == DirectoryState::UNCACHED)
         {
            processExReqFromL2Cache(shmem_req);
         }
      }
      else // (shmem_req->getShmemMsg()->m_msg_type == ShmemMsg::SH_REQ)
      {
         // A ShmemMsg::SH_REQ caused the invalidation
         processShReqFromL2Cache(shmem_req);
      }
   }
}

void
DramDirectoryCntlr::processFlushRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->m_address;

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   assert(directory_block_info->getDState() == DirectoryState::EXCLUSIVE);

   directory_entry->removeSharer(sender);
   directory_entry->setOwner(INVALID_CORE_ID);
   directory_block_info->setDState(DirectoryState::UNCACHED);

   m_dram_cntlr->putDataToDram(address, shmem_msg->m_data_buf);

   if (m_dram_directory_req_queue_list->size(address) != 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);
      // An involuntary/voluntary Flush
      if (shmem_req->getShmemMsg()->m_msg_type == ShmemMsg::EX_REQ)
      {
         processExReqFromL2Cache(shmem_req);
      }
      else // (shmem_req->getShmemMsg()->m_msg_type == ShmemMsg::SH_REQ)
      {
         processShReqFromL2Cache(shmem_req);
      }
   }
}

void
DramDirectoryCntlr::processWbRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->m_address;

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();

   LOG_PRINT("processWbRepFromL2Cache: Starting assertions");
   assert(directory_block_info->getDState() == DirectoryState::EXCLUSIVE);

   assert(directory_entry->hasSharer(sender));
   LOG_PRINT("processWbRepFromL2Cache: Ending assertions");

   LOG_PRINT("processWbRepFromL2Cache: Starting to set directory info");
   directory_entry->setOwner(INVALID_CORE_ID);
   directory_block_info->setDState(DirectoryState::SHARED);
   LOG_PRINT("processWbRepFromL2Cache: Finished setting directory info");

   m_dram_cntlr->putDataToDram(address, shmem_msg->m_data_buf);

   if (m_dram_directory_req_queue_list->size(address) != 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);
      assert(shmem_req->getShmemMsg()->m_msg_type == ShmemMsg::SH_REQ);
      processShReqFromL2Cache(shmem_req);
   }
}

}