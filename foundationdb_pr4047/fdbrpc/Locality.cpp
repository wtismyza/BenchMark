/*
 * Locality.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbrpc/Locality.h"

const UID LocalityData::UNSET_ID = UID(0x0ccb4e0feddb5583, 0x010f6b77d9d10ece);
const StringRef LocalityData::keyProcessId = LiteralStringRef("processid");
const StringRef LocalityData::keyZoneId = LiteralStringRef("zoneid");
const StringRef LocalityData::keyDcId = LiteralStringRef("dcid");
const StringRef LocalityData::keyMachineId = LiteralStringRef("machineid");
const StringRef LocalityData::keyDataHallId = LiteralStringRef("data_hall");

ProcessClass::Fitness ProcessClass::machineClassFitness( ClusterRole role ) const {
	switch( role ) {
	case ProcessClass::Storage:
		switch( _class ) {
		case ProcessClass::StorageClass:
			return ProcessClass::BestFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::WorstFit;
		case ProcessClass::LogClass:
			return ProcessClass::WorstFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::NeverAssign;
		}
	case ProcessClass::TLog:
		switch( _class ) {
		case ProcessClass::LogClass:
			return ProcessClass::BestFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::StorageClass:
			return ProcessClass::WorstFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::NeverAssign;
		}
	case ProcessClass::Proxy:
		switch( _class ) {
		case ProcessClass::ProxyClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::ResolutionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Master:
		switch( _class ) {
		case ProcessClass::MasterClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::ResolutionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Resolver:
		switch( _class ) {
		case ProcessClass::ResolutionClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::LogRouter:
		switch( _class ) {
		case ProcessClass::LogRouterClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::ResolutionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::ClusterController:
		switch( _class ) {
		case ProcessClass::ClusterControllerClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::MasterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::ResolutionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::TransactionClass:
			return ProcessClass::OkayFit;
		case ProcessClass::ProxyClass:
			return ProcessClass::OkayFit;
		case ProcessClass::LogRouterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
			return ProcessClass::NeverAssign;
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::DataDistributor:
		switch( _class ) {
		case ProcessClass::DataDistributorClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::MasterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	case ProcessClass::Ratekeeper:
		switch( _class ) {
		case ProcessClass::RatekeeperClass:
			return ProcessClass::BestFit;
		case ProcessClass::StatelessClass:
			return ProcessClass::GoodFit;
		case ProcessClass::UnsetClass:
			return ProcessClass::UnsetFit;
		case ProcessClass::MasterClass:
			return ProcessClass::OkayFit;
		case ProcessClass::CoordinatorClass:
		case ProcessClass::TesterClass:
			return ProcessClass::NeverAssign;
		default:
			return ProcessClass::WorstFit;
		}
	default:
		return ProcessClass::NeverAssign;
	}
}

LBDistance::Type loadBalanceDistance( LocalityData const& loc1, LocalityData const& loc2, NetworkAddress const& addr2 ) {
	if ( FLOW_KNOBS->LOAD_BALANCE_ZONE_ID_LOCALITY_ENABLED && loc1.zoneId().present() && loc1.zoneId() == loc2.zoneId() ) {
		return LBDistance::SAME_MACHINE;
	}
	//FIXME: add this back in when load balancing works with local requests
	//if ( g_network->isAddressOnThisHost( addr2 ) )
	//	return LBDistance::SAME_MACHINE;
	if ( FLOW_KNOBS->LOAD_BALANCE_DC_ID_LOCALITY_ENABLED && loc1.dcId().present() && loc1.dcId() == loc2.dcId() ) {
		return LBDistance::SAME_DC;
	}
	return LBDistance::DISTANT;
}
