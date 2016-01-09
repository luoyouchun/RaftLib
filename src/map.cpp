/**
 * map.cpp - 
 * @author: Jonathan Beard
 * @version: Fri Sep 12 10:28:33 2014
 * 
 * Copyright 2014 Jonathan Beard
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <sstream>
#include <map>
#include <sstream>
#include <vector>
#include <typeinfo>
#include <cstdio>
#include <cassert>
#include <string>
#include <cstdlib>
#include <cstring>

#include "common.hpp"
#include "map.hpp"
#include "graphtools.hpp"
#include "kpair.hpp"

raft::map::map() : MapBase()
{
  
}

raft::map::~map()
{
}

void
raft::map::checkEdges( kernelkeeper &source_k )
{
   auto &container( source_k.acquire() );
   /**
    * NOTE: will throw an error that we're not catching here
    * if there are unconnected edges...this is something that
    * a user will have to fix.  Otherwise will return with no
    * errors.
    */
   GraphTools::BFS( container, 
                    []( PortInfo &a, PortInfo &b, void *data ){ return; },
                    nullptr,
                    true );
   source_k.release();
   return;
}

/**
   void insert( raft::kernel &a,  PortInfo &a_out, 
                raft::kernel &b,  PortInfo &b_in,
                raft::kernel &i,  PortInfo &i_in, PortInfo &i_out );
**/
void 
raft::map::enableDuplication( kernelkeeper &source, kernelkeeper &all )
{
    auto &source_k( source.acquire() );
    auto &all_k   ( all.acquire()    );
    /** don't have to do this but it makes it far more apparent where it comes from **/
    void * const kernel_ptr( reinterpret_cast< void* >( &all_k ) );
    using kernel_ptr_t = 
      typename std::remove_reference< decltype( all_k ) >::type;
    /** need to grab impl of Lengauer and Tarjan dominators, use for SESE **/
    /** in the interim, restrict to kernels that are simple to duplicate **/
    GraphTools::BFS( source_k,
                     []( PortInfo &a, PortInfo &b, void *data )
                     {
                        auto * const all_k( reinterpret_cast< kernel_ptr_t* >( data ) );  
                        if( a.out_of_order && b.out_of_order )
                        {
                           /** case of inline kernel **/
                           if( b.my_kernel->input.count() == 1 && 
                               b.my_kernel->output.count() == 1 && 
                               a.my_kernel->dup_candidate  )
                           {
                              auto *kernel_a( a.my_kernel );
                              assert( kernel_a->input.count() == 1 );
                              auto &port_info_front( kernel_a->input.getPortInfo() );
                              auto *front( port_info_front.other_kernel );
                              auto &front_port_info( front->output.getPortInfo() );
                              /**
                               * front -> kernel_a goes to
                               * front -> split -> kernel_a 
                               */
                              auto *split( 
                                 static_cast< raft::kernel* >( 
                                    port_info_front.split_func() ) );
                              all_k->insert( split );
                              MapBase::insert( front,    front_port_info,
                                               kernel_a, port_info_front,
                                               split );

                              assert( kernel_a->output.count() == 1 );

                              /** 
                               * now we need the port info from the input 
                               * port on back 
                               **/

                              /**
                               * kernel_a -> back goes to
                               * kernel_a -> join -> back 
                               */
                              auto *join( static_cast< raft::kernel* >( a.join_func() ) );
                              all_k->insert( join );
                              MapBase::insert( a.my_kernel, a,
                                               b.my_kernel, b,
                                               join );
                              /** 
                               * finally set the flag to the scheduler
                               * so that the parallel map manager can
                               * pick it up an use it.
                               */
                              a.my_kernel->dup_enabled = true; 
                           }
                           /** parallalizable source, single output no inputs**/
                           else if( a.my_kernel->input.count() == 0 and
                                    a.my_kernel->output.count() == 1 )
                           {
                              auto *join( static_cast< raft::kernel* >( a.join_func() ) );
                              all_k->insert( join );
                              MapBase::insert( a.my_kernel, a,
                                               b.my_kernel, b,
                                               join );
                              a.my_kernel->dup_enabled = true; 
                           }
                           /** parallelizable sink, single input, no outputs **/
                           else if( b.my_kernel->input.count() == 1 and
                                    b.my_kernel->output.count() == 0 )
                           {
                              auto *split( 
                                 static_cast< raft::kernel* >( b.split_func() ) );
                              all_k->insert( split );
                              MapBase::insert( a.my_kernel, a,
                                               b.my_kernel, b,
                                               split );
                              b.my_kernel->dup_enabled = true;
                           }
                           /** 
                            * flag as candidate if the connecting
                            * kernel only has one input port.
                            */
                           else if( b.my_kernel->input.count() == 1 )
                           {
                              /** simply flag as a candidate **/
                              b.my_kernel->dup_candidate = true;
                           }
                              
                        }
                     },
                     kernel_ptr,
                     false );
   source.release();
   all.release();
}

void 
raft::map::joink( kpair * const next )
{
        /** might be able to do better by re-doing with templates **/
        if( next->has_src_name && next->has_dst_name )
        {
            (this)->link( next->src, next->src_name,
                          next->dst, next->dst_name );
        }
        else if( next->has_src_name && ! next->has_dst_name )
        {
            (this)->link( next->src, next->src_name, next->dst );
        }
        else if( ! next->has_src_name && next->has_dst_name )
        {
            (this)->link( next->src, next->dst, next->dst_name );
        }
        else /** single input, single output, hopefully **/
        {
            (this)->link( next->src, next->dst );
        }
}

kernel_pair_t
raft::map::operator += ( kpair * const pair )
{
    assert( pair != nullptr );
    raft::kernel *end( pair->dst ); 
    /** start at the head, go forward **/
    kpair *next = pair->head;
    assert( next != nullptr );
    raft::kernel *start( next->src );
    bool found_split_to( false );
    bool do_join( false );
    std::uint32_t split_count( 0 );
    std::vector< raft::kernel* > kernels;
    while( next != nullptr )
    {
        if( next->split_to )
        {
            found_split_to = true;
            /** get source count **/
            split_count = next->src_out_count;
            /** 
             * first one allocated somewhere else, the 
             * rest we'll have to allocate ourselves,
             * don't worry, the mapbase will delete
             * them.
             */
        }
        if( next->join_from )
        {
            assert(found_split_to  == true );
            found_split_to = false;
            do_join = true;
            //FIXME, throw an exception here
            assert( split_count == next->dst_in_count );
        }
        
        if( ! found_split_to  && ! do_join )
        {
            joink( next );        
        }
        else if( found_split_to )
        {
            if( kernels.size() == 0 )
            {
                //FIXME, should access port name for src
                /** join first kernel **/
                kernels.emplace_back( next->dst );
                (this)->link( next->src, "0", next->dst );
                /** duplicate and join to first split **/
                for( auto i( 1 ); i < split_count; i++ )
                {
                    raft::kernel *temp_k( next->dst->clone() );
                    kernels.push_back( temp_k );
                    /** these should all be one-one **/
                    (this)->link( next->src, 
                                  std::to_string( i ),
                                  kernels[ i ] );
                }
            }
            else
            {
                std::vector< raft::kernel* > temp;
                /** duplicate and join to kernels in vector **/
                (this)->link( kernels[ 0 ], next->dst );
                for( auto i( 1 ); i < split_count; i++ )
                {
                    raft::kernel *temp_k( next->dst->clone() );
                    temp.push_back( temp_k );
                    (this)->link( kernels[ i ], temp[ i ] );
                }
                kernels.clear();
                kernels.insert( kernels.end(), temp.begin(), temp.end() );
            }
        }
        else if( do_join )
        {
            /** kernels should be full **/
            assert( kernels.size() > 0 );
            auto *dst( next->dst );
            //no clones
            for( auto i( 0 ); i < split_count; i++ )
            {
                (this)->link( kernels[ i ], dst, std::to_string( i ) );
            }
        }
        
        kpair *temp = next;
        next = next->next;
        delete( temp );
    }
    return( kernel_pair_t( start, end ) );
}