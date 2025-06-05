// Google Apps Script for ESP8266 Datalogger

// --- Configuration ---
const SPREADSHEET_ID = "10LiI4mE2pXZIAGsXhWJl5giGbgp0ytBHyS3JcWaoyTA";
const SHEET_NAME = "Raw Data"; // The name of the sheet/tab to log data to

// Define the order of sensor data keys as expected from the ESP8266 and for sheet columns
const SENSOR_DATA_KEYS = [
  "sensor1",
  "sensor2",
  "sensor3",
  "sensor4",
  "dhttemp",
  "dhthumidity"
];

/**
 * Handles HTTP POST requests from the ESP8266.
 * @param {Object} e The event parameter for a POST request.
 * @return {ContentService.TextOutput} A text output indicating success or failure.
 */
function doPost(e) {
  try {
    // Log the raw content of the POST request for debugging
    Logger.log("Received POST data: " + (e && e.postData ? e.postData.contents : "No postData found"));

    if (!e || !e.postData || !e.postData.contents) {
      Logger.log("Error: No data received in POST request.");
      return ContentService.createTextOutput("Error: No data received in POST request.")
                           .setMimeType(ContentService.MimeType.TEXT);
    }

    // Attempt to open the specific spreadsheet and sheet
    let spreadsheet;
    try {
      spreadsheet = SpreadsheetApp.openById(SPREADSHEET_ID);
    } catch (err) {
      Logger.log("Error opening spreadsheet by ID '" + SPREADSHEET_ID + "': " + err);
      return ContentService.createTextOutput("Error: Could not open spreadsheet. Check SPREADSHEET_ID.")
                           .setMimeType(ContentService.MimeType.TEXT);
    }

    const sheet = spreadsheet.getSheetByName(SHEET_NAME);
    if (!sheet) {
      Logger.log("Error: Sheet named '" + SHEET_NAME + "' not found.");
      return ContentService.createTextOutput("Error: Sheet '" + SHEET_NAME + "' not found.")
                           .setMimeType(ContentService.MimeType.TEXT);
    }

    // Parse the JSON payload from the ESP8266
    const payload = JSON.parse(e.postData.contents);
    Logger.log("Parsed payload: " + JSON.stringify(payload));

    // Timestamp for when the data is received and processed by the script.
    // For more accuracy, the ESP8266 could send its own NTP-synced timestamp.
    const timestamp = new Date();

    // Prepare the row data for the spreadsheet
    // The first column will be the timestamp.
    const rowData = [timestamp];

    // Process each expected sensor key
    for (const key of SENSOR_DATA_KEYS) {
      let value = payload[key];

      // Handle cases where ESP8266 might send "nan" (string) for float NAN values,
      // or if the value is otherwise not a valid number.
      if (typeof value === 'string' && value.toLowerCase() === 'nan') {
        rowData.push(null); // Store 'null' in the sheet for "nan" strings
      } else if (typeof value === 'number' && !isNaN(value)) {
        rowData.push(value); // Valid number, store as is (includes 0)
      } else if (value === undefined || value === null) {
        rowData.push(null); // Key was missing from payload or explicitly null
      } else {
        // If the value is something unexpected (e.g., a different string), log it and store null.
        Logger.log("Warning: Unexpected value for key '" + key + "': " + value + " (Type: " + typeof value + "). Storing as null.");
        rowData.push(null);
      }
    }

    // Append the processed row to the sheet
    sheet.appendRow(rowData);
    Logger.log("Successfully appended row to sheet '" + SHEET_NAME + "': " + JSON.stringify(rowData));

    return ContentService.createTextOutput("Success: Data logged to " + SHEET_NAME)
                         .setMimeType(ContentService.MimeType.TEXT);

  } catch (error) {
    // Log the detailed error
    Logger.log("Error in doPost: " + error.toString() + "\nStack: " + error.stack);
    // Also log the incoming data that might have caused the error, if available
    if (e && e.postData && e.postData.contents) {
      Logger.log("Failed on payload: " + e.postData.contents);
    }
    return ContentService.createTextOutput("Error processing request: " + error.toString())
                         .setMimeType(ContentService.MimeType.TEXT);
  }
}

// Optional: A simple function to test setup from the Apps Script editor
function testSheetAccess() {
  try {
    const ss = SpreadsheetApp.openById(SPREADSHEET_ID);
    Logger.log("Spreadsheet Name: " + ss.getName());
    const sheet = ss.getSheetByName(SHEET_NAME);
    if (sheet) {
      Logger.log("Test sheet '" + sheet.getName() + "' found and accessible.");
      // Example: Add a test header if sheet is empty
      // if (sheet.getLastRow() === 0) {
      //   sheet.appendRow(["Timestamp", "Sensor1", "Sensor2", "Sensor3", "Sensor4", "DHT Temp", "DHT Humidity"]);
      //   Logger.log("Added header row to empty sheet.");
      // }
    } else {
      Logger.log("Error: Test sheet '" + SHEET_NAME + "' not found in spreadsheet ID: " + SPREADSHEET_ID);
    }
  } catch (err) {
    Logger.log("Error in testSheetAccess: " + err.toString());
  }
}