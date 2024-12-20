/*
 * Vermont Configuration Subsystem
 * Copyright (C) 2009 Vermont Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "AggregatorBaseCfg.h"
#include "Rule.hpp"
#include "Rules.hpp"
#include "core/XMLElement.h"
#include "core/InfoElementCfg.h"
#include "BaseHashtable.h"

AggregatorBaseCfg::AggregatorBaseCfg(XMLElement* elem)
	: CfgBase(elem), pollInterval(0)
{
	if (!elem)
		return;

	rules = new Rules;
	htableBits = HT_DEFAULT_BITSIZE;

	XMLNode::XMLSet<XMLElement*> set = elem->getElementChildren();
	for (XMLNode::XMLSet<XMLElement*>::iterator it = set.begin();
	     it != set.end();
	     it++) {
		XMLElement* e = *it;
		if (e->matches("rule")) {
			Rule* r = readRule(e);
			if (r) {
				if (rules->count < MAX_RULES) {
					rules->rule[rules->count++] = r;
				} else {
					msg(LOG_CRIT, "Too many rules: %ul\n", MAX_RULES);
				}
				
			}
		} else if (e->matches("expiration")) {
			// get the time values or set them to '0' if they are not specified
			activeTimeout = getTimeInUnit("activeTimeout", SEC, 0, e);
			inactiveTimeout = getTimeInUnit("inactiveTimeout", SEC, 0, e);
		} else if (e->matches("pollInterval")) {
			pollInterval = getTimeInUnit("pollInterval", mSEC, AGG_DEFAULT_POLLING_TIME);
		} else if (e->matches("hashtableBits")) {
			htableBits = getInt("hashtableBits", HT_DEFAULT_BITSIZE);
		} else if (e->matches("next")) { // ignore next
		} else {
			msg(LOG_CRIT, "Unkown Aggregator config entry %s\n", e->getName().c_str());
		}
	}
}

AggregatorBaseCfg::~AggregatorBaseCfg()
{
}

Rule* AggregatorBaseCfg::readRule(XMLElement* elem) {
	// nonflowkey -> aggregate
	// flowkey -> keep

	Rule* rule = new Rule();
	XMLNode::XMLSet<XMLElement*> set = elem->getElementChildren();
	for (XMLNode::XMLSet<XMLElement*>::iterator it = set.begin();
	     it != set.end();
	     it++) {
		XMLElement* e = *it;

		if (e->matches("templateId")) {
			rule->id = getInt("templateId", 0, e);
		} else if (e->matches("biflowAggregation")) {
			rule->biflowAggregation = getInt("biflowAggregation", 0, e);
		} else if (e->matches("flowKey")) {
			Rule::Field* ruleField = readFlowKeyRule(e);
			if (ruleField) {
				if (rule->fieldCount < MAX_RULE_FIELDS) {
					rule->field[rule->fieldCount++] = ruleField;
				} else {
					THROWEXCEPTION("Too many rule fields (%d)", MAX_RULE_FIELDS);
				}
			}
		} else if (e->matches("nonFlowKey")) {
			Rule::Field* ruleField = readNonFlowKeyRule(e);
			if (ruleField) {
				if (rule->fieldCount < MAX_RULE_FIELDS) {
					rule->field[rule->fieldCount++] = ruleField;
				} else {
					THROWEXCEPTION("Too many rule fields (%d)", MAX_RULE_FIELDS);
				}
			}
		} else {
			THROWEXCEPTION("Unknown rule %s in Aggregator found", e->getName().c_str());
		}
	}

	// we found no rules, so do cleanup
	if (rule->fieldCount == 0) {
		delete rule;
		rule = NULL;
	} else {
		// exclude coexistence of patterns and biflow aggregation
		if(rule->biflowAggregation) {
			for(int i=0; i < rule->fieldCount; i++) {
				if(rule->field[i]->pattern) {
					msg(LOG_ERR, "AggregatorBaseCfg: Match pattern for id=%d ignored because biflow aggregation is enabled.", rule->field[i]->type.id);
					free(rule->field[i]->pattern);
					rule->field[i]->pattern = NULL;
				}
			}
		}
	}
	return rule;
}

Rule::Field* AggregatorBaseCfg::readNonFlowKeyRule(XMLElement* e)
{
	Rule::Field* ruleField = new Rule::Field();
	InfoElementCfg ie(e);

	if (!ie.isKnownIE())
		THROWEXCEPTION("Unsupported non-key field %s (id=%u, enterprise=%u).", (ie.getIeName()).c_str(), ie.getIeId(), ie.getEnterpriseNumber());

	ruleField->modifier = Rule::Field::AGGREGATE;
	ruleField->type.id = ie.getIeId();
	ruleField->type.enterprise = ie.getEnterpriseNumber();
	ruleField->type.length = ie.getIeLength();

	ruleField->semantic = ie.getSemantic();
	ruleField->fieldIe = ie.getFieldIe();

	if (!BaseHashtable::isToBeAggregated(ruleField->type)) {
		msg(LOG_ERR, "Field %s configured as nonFlowKey will not be aggregated", ie.getIeName().c_str());
	}

	if (ie.getAutoAddV4PrefixLength() &&
			(ruleField->type == InformationElement::IeInfo(IPFIX_TYPEID_sourceIPv4Address, 0) ||
			ruleField->type == InformationElement::IeInfo(IPFIX_TYPEID_destinationIPv4Address, 0))) {
		ruleField->type.length++; // for additional mask field
	}

	if (ruleField->type == InformationElement::IeInfo(IPFIX_ETYPEID_frontPayload, IPFIX_PEN_vermont) ||
			ruleField->type == InformationElement::IeInfo(IPFIX_ETYPEID_frontPayload, IPFIX_PEN_vermont|IPFIX_PEN_reverse)) {
		if (ruleField->type.length<5)
			THROWEXCEPTION("type %s must have at least size 5!", ipfix_id_lookup(ruleField->type.id, ruleField->type.enterprise)->name);
	}

	return ruleField;
}

Rule::Field* AggregatorBaseCfg::readFlowKeyRule(XMLElement* e) {
	Rule::Field* ruleField = new Rule::Field();
	InfoElementCfg ie(e);

	if (!ie.isKnownIE())
		THROWEXCEPTION("Unknown flow key field %s (id=%u, enterprise=%u).", (ie.getIeName()).c_str(), ie.getIeId(), ie.getEnterpriseNumber());

	try {   // TODO: not sure why we don't abort here; just use the same code as before restructuring
		// I added a delete ruleField in the catch, perhaps thats why it was needed?

		// parse modifier
		if (ie.getModifier().empty() || (ie.getModifier() == "keep")) {
			ruleField->modifier = Rule::Field::KEEP;
		} else if (ie.getModifier() == "discard") {
			ruleField->modifier = Rule::Field::DISCARD;
		} else {
			ruleField->modifier = (Rule::Field::Modifier)((int)Rule::Field::MASK_START + atoi(ie.getModifier().c_str() + 5));
		}

		ruleField->type.id = ie.getIeId();
		ruleField->type.enterprise = ie.getEnterpriseNumber();
		ruleField->type.length = ie.getIeLength();

		if (BaseHashtable::isToBeAggregated(ruleField->type)) {
			msg(LOG_ERR, "Field %s configured as FlowKey will be aggregated", ie.getIeName().c_str());
		}

		if (ie.getAutoAddV4PrefixLength() &&
				(ruleField->type.id == IPFIX_TYPEID_sourceIPv4Address || ruleField->type.id == IPFIX_TYPEID_destinationIPv4Address)) {
			ruleField->type.length++; // for additional mask field
		}

		if (!ie.getMatch().empty()) {
			// TODO: we need to copy the string because parseProtoPattern and
			//  parseIPv4Pattern violate the original string
			char* tmp = new char[ie.getMatch().length() + 1];
			strcpy(tmp, ie.getMatch().c_str());
			ruleField->pattern = NULL;

			switch (ruleField->type.id) {
			case IPFIX_TYPEID_protocolIdentifier:
				if (parseProtoPattern(tmp, &ruleField->pattern, &ruleField->type.length) != 0) {
					msg(LOG_ERR, "Bad protocol pattern \"%s\"", tmp);
					delete [] tmp;
					throw std::exception();
				}
				break;
			case IPFIX_TYPEID_sourceMacAddress:
			case IPFIX_TYPEID_destinationMacAddress:
				if (parseMacAddressPattern(tmp, &ruleField->pattern, &ruleField->type.length) !=0) {
					msg(LOG_ERR, "Bad Mac Address pattern \"%s\"", tmp);
					delete [] tmp;
					throw std::exception();
				}
				break;
			case IPFIX_TYPEID_sourceIPv4Address:
			case IPFIX_TYPEID_destinationIPv4Address:
				if (parseIPv4Pattern(tmp, &ruleField->pattern, &ruleField->type.length) != 0) {
					msg(LOG_ERR, "Bad IPv4 pattern \"%s\"", tmp);
					delete [] tmp;
					throw std::exception();
				}
				break;
			case IPFIX_TYPEID_sourceTransportPort:
			case IPFIX_TYPEID_udpSourcePort:
			case IPFIX_TYPEID_tcpSourcePort:
			case IPFIX_TYPEID_destinationTransportPort:
			case IPFIX_TYPEID_udpDestinationPort:
			case IPFIX_TYPEID_tcpDestinationPort:
				if (parsePortPattern(tmp, &ruleField->pattern, &ruleField->type.length) != 0) {
					msg(LOG_ERR, "Bad PortRanges pattern \"%s\"", tmp);
					delete [] tmp;
					throw std::exception();
				}
				break;
			case IPFIX_TYPEID_tcpControlBits:
				if (parseTcpFlags(tmp, &ruleField->pattern, &ruleField->type.length) != 0) {
					msg(LOG_ERR, "Bad TCP flags pattern \"%s\"", tmp);
					delete [] tmp;
					throw std::exception();
				}
				break;

			default:
				msg(LOG_ERR, "Fields of type \"%s\" cannot be matched against a pattern %s", "", tmp);
				delete [] tmp;
				throw std::exception();
				break;
			}
			delete [] tmp;
		}
	} catch (std::exception& e) {
		delete ruleField;
		ruleField = NULL;
	}


	return ruleField;
}

bool AggregatorBaseCfg::equalTo(AggregatorBaseCfg *other) {
	if (activeTimeout != other->activeTimeout) return false;
	if (inactiveTimeout != other->inactiveTimeout) return false;
	if (pollInterval != other->pollInterval) return false;
	if (htableBits != other->htableBits) return false;
	if (*rules != *other->rules) return false;

	return true;
}
